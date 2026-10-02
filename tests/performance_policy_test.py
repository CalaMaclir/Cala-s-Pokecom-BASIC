"""Deterministic off/report, warning, semantic-error and measurement-error regressions."""
import copy
import contextlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import performance_policy as policy
import benchmark_correctness
import ci_validation_mode
import stage2_evidence
import stage3_evidence
import paired_benchmark


def case(name):
    return dict(name=name, output_hash=1, graphics_hash=2, pixels=0, ops=3,
                dispatches=4, logical_ops=5, op_counts=[1,0],
                **{m+'_raw_us':[100.0]*9 for m in ('compile','vm','cache_miss','cache_hit')})

class PolicyTests(unittest.TestCase):
    def test_warning_never_fails(self):
        b,c,d=policy.compare_samples([100.]*9,[115.]*9)
        self.assertAlmostEqual(d,15)
        r=policy.outcome([{'delta_percent':d}])
        self.assertEqual((r['performance_status'],r['correctness_status'],r['acceptance_gate']),('WARN','PASS','WARN'))
        self.assertEqual(len(r['timing_alerts']),1)
        self.assertEqual(policy.outcome([{'delta_percent':0}])['performance_status'],'PASS')

    def test_bad_samples_are_errors(self):
        for samples in ([],[1]*8,[0]*9,[-1]*9,[math.nan]*9,[math.inf]*9,[True]*9,['bad']*9):
            with self.subTest(samples=samples),self.assertRaises(ValueError):
                policy.compare_samples([100.]*9,samples)
        with self.assertRaises(ValueError):policy.compare_samples([0]*9,[115]*9)
        with self.assertRaises(ValueError):policy.outcome([])

    def test_semantics_and_missing_data(self):
        fixture=json.loads((Path(__file__).parent/'benchmark_expected.json').read_text())
        data={'cases':[dict(name=n,**v) for n,v in fixture['cases'].items()]}
        benchmark_correctness.validate(data,fixture)
        for key in ('output_hash','graphics_hash','pixels'):
            bad=copy.deepcopy(data);bad['cases'][0][key]+=1
            with self.assertRaises(policy.SemanticMismatch):benchmark_correctness.validate(bad,fixture)
        bad=copy.deepcopy(data);bad['cases'].pop()
        with self.assertRaises(policy.SemanticMismatch):benchmark_correctness.validate(bad,fixture)
        bad=copy.deepcopy(data);del bad['cases'][0]['output_hash']
        with self.assertRaises(KeyError):benchmark_correctness.validate(bad,fixture)

    def test_off_is_not_pass(self):
        with tempfile.TemporaryDirectory() as d:
            r=policy.load_summary(Path(d),'off')
            self.assertEqual(r['performance_status'],'NOT_RUN')
            self.assertEqual(r['acceptance_gate'],'NOT_RUN')
            self.assertEqual(r['measurement_status'],'NOT_RUN')
            with self.assertRaises(FileNotFoundError):policy.load_summary(Path(d),'report')
            (Path(d)/'stage2-evidence.json').write_text('invalid json')
            with self.assertRaises(json.JSONDecodeError):policy.load_summary(Path(d),'report')

    def test_error_is_not_success(self):
        self.assertEqual(policy.error_record(ValueError('bad'))['performance_status'],'ERROR')
        self.assertEqual(policy.error_record(policy.SemanticMismatch('bad'))['correctness_status'],'FAIL')
        # Actual CLI must retain ERROR and exit nonzero on missing measured evidence.
        with tempfile.TemporaryDirectory() as d:
            r=subprocess.run([sys.executable,str(Path(policy.__file__)), '--mode','report','--directory',d],capture_output=True)
            self.assertNotEqual(r.returncode,0)
            self.assertEqual(json.loads((Path(d)/'performance-status.json').read_text())['acceptance_gate'],'ERROR')

    def test_benchmark_crash_is_error(self):
        with tempfile.TemporaryDirectory() as d:
            binary=Path(d)/'crash'
            binary.write_text('#!/bin/sh\nexit 13\n');binary.chmod(0o755)
            previous=Path.cwd()
            try:
                os.chdir(d)
                with patch.object(paired_benchmark,'fixed_address_layout',lambda:None):
                    with self.assertRaises(RuntimeError):
                        paired_benchmark._measure({'Current':str(binary)},{'fixture.bas':('Current',)})
            finally:
                os.chdir(previous)

    def test_mode_selection(self):
        self.assertEqual(ci_validation_mode.performance_mode({'GITHUB_EVENT_NAME':'push'}),'off')
        self.assertEqual(ci_validation_mode.performance_mode({'GITHUB_EVENT_NAME':'workflow_dispatch','CI_PERFORMANCE_MODE':'report'}),'report')
        with self.assertRaises(ValueError):ci_validation_mode.performance_mode({'GITHUB_EVENT_NAME':'workflow_dispatch','CI_PERFORMANCE_MODE':'unknown'})
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'event.json'
            for labels,mode in (([], 'off'),([{'name':'performance-report'}],'report')):
                p.write_text(json.dumps({'pull_request':{'labels':labels}}))
                self.assertEqual(ci_validation_mode.performance_mode({'GITHUB_EVENT_NAME':'pull_request','GITHUB_EVENT_PATH':str(p)}),mode)

    @unittest.skipUnless((Path(__file__).resolve().parents[1]/'.github/workflows/build.yml').is_file(),
                         'Private build workflow topology is not part of the public source distribution')
    def test_workflow_dependencies(self):
        root=Path(__file__).resolve().parents[1]
        text=(root/'.github/workflows/build.yml').read_text()
        steps=[]
        for block in text.split('      - name: ')[1:]:
            lines=block.splitlines()
            step={'name':lines[0]}
            for line in lines[1:]:
                if line.startswith('        if: '):step['if']=line.split('if: ',1)[1]
                if 'continue-on-error:' in line:step['continue-on-error']=line
            steps.append(step)
        for step in steps:
            if step['name'].startswith(('Checkout Stage','Checkout frozen Stage','Checkout accepted Stage','Measure Stage')):
                self.assertIn("env.PERFORMANCE_MODE == 'report'",step['if'])
                self.assertNotIn('continue-on-error',step)
        for name in ('Build cached host tests and run all regressions serially','Verify ARM numeric-local fusion does not contract floating operations','Measure SRAM and stack usage','Record firmware identity and verify complete host results','Stage and audit complete release package','Upload firmware and memory evidence'):
            step=next(s for s in steps if s['name']==name)
            self.assertNotIn('PERFORMANCE_MODE',step.get('if',''))
        self.assertIn('performance_mode:\n        description:',text)
        self.assertIn('default: off',text)

    def test_stage2_warning_and_corruption(self):
        names=['examples/stage3/arithmetic-classic.bas','examples/stage3/numeric-classic.bas','examples/mandel_text.bas']
        original={'Stage 1':[{'cases':[case(n) for n in names]}],'Current':[{'cases':[case(n) for n in names]}]}
        for c in original['Current'][0]['cases']:
            c['compile_raw_us']=[115.]*9;c['vm_raw_us']=[115.]*9
        with tempfile.TemporaryDirectory() as d:
            p=Path(d); mem=p/'mem';mem.write_text('.data 10 0\n.bss 20 0\n')
            argv=['stage2','--baseline','baseline','--current','current','--baseline-memory',str(mem),'--current-memory',str(mem),'--output',str(p/'result')]
            for corrupt in (None,'output_hash','compile_raw_us'):
                runs=copy.deepcopy(original)
                if corrupt=='output_hash':runs['Current'][0]['cases'][0]['output_hash']=99
                if corrupt=='compile_raw_us':runs['Current'][0]['cases'][0]['compile_raw_us']=[math.nan]*9
                with patch.object(sys,'argv',argv),patch('paired_benchmark.environment',return_value={}),patch('paired_benchmark.measure',return_value=(runs,[])),patch.object(stage2_evidence,'verify_source'):
                    if corrupt:
                        with self.assertRaises((policy.SemanticMismatch,ValueError)):stage2_evidence.main()
                    else:
                        with contextlib.redirect_stdout(io.StringIO()):stage2_evidence.main()
                        result=json.loads((p/'result.json').read_text())
                        self.assertEqual(policy.validate_evidence(result),'WARN')
                        self.assertTrue(result['failures']) # retained historical advisory field
                        bad=copy.deepcopy(result);bad['baseline_sha']='wrong'
                        with self.assertRaises(policy.SemanticMismatch):policy.validate_evidence(bad)
                        bad=copy.deepcopy(result);bad['comparisons'][0]['baseline']+=1
                        with self.assertRaises(policy.SemanticMismatch):policy.validate_evidence(bad)

    def test_stage3_warning_and_corruption(self):
        classic=['examples/mandel_text.bas','examples/picocalc_mand.bas']+[f'examples/stage3/{g}-classic.bas' for g in ('arithmetic','numeric','fractal')]
        allnames=classic+[f'examples/stage3/{g}-{m}.bas' for g in ('arithmetic','numeric','fractal') for m in ('colon','rows','function')]+['examples/stage3/tiny-function.bas']
        original={n:[{'cases':[case(c) for c in (classic if n=='Stage 1' else allnames)]}] for n in ('Stage 1','Stage 2','Accepted 3AB','Current')}
        for c in original['Current'][0]['cases']:
            for m in ('compile','vm','cache_miss','cache_hit'):c[m+'_raw_us']=[115.]*9
        read_text=Path.read_text
        def read(path,*args,**kwargs):
            if str(path)=='include/il.hpp':return 'enum class OpCode { CALL_USER, LOCAL_NUM_FUSED };'
            return read_text(path,*args,**kwargs)
        def dump(args,**kwargs):
            if '--dump-section' in args:Path(args[2].split('=',1)[1]).write_bytes(b'identical')
        def command(args,**kwargs):return 'fixture' if args[0] in ('git','g++') else ''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);mem=p/'mem';mem.write_text('.data 10 0\n.bss 20 0\n')
            binary=p/'binary';binary.write_bytes(b'binary')
            argv=['stage3']
            for n in ('stage1','stage2','current','accepted'):argv+=['--'+n,str(binary)]
            for n in ('stage1-memory','stage2-memory','current-memory','accepted-memory'):argv+=['--'+n,str(mem)]
            argv+=['--output',str(p/'result')]
            for corrupt in (None,'graphics_hash','cache_hit_raw_us'):
                runs=copy.deepcopy(original)
                if corrupt=='graphics_hash':runs['Current'][0]['cases'][0]['graphics_hash']=99
                if corrupt=='cache_hit_raw_us':runs['Current'][0]['cases'][0]['cache_hit_raw_us']=[]
                with patch.object(sys,'argv',argv),patch.object(stage3_evidence,'verify_source'),patch.object(stage3_evidence,'environment',return_value={}),patch.object(stage3_evidence,'measure',return_value=(runs,[])),patch('subprocess.check_output',side_effect=command),patch('subprocess.run',side_effect=dump),patch.object(Path,'read_text',read),patch.dict(os.environ,{'CPB_CALL_USER_OPCODE':'0'}):
                    if corrupt:
                        with self.assertRaises((policy.SemanticMismatch,ValueError)):stage3_evidence.main()
                    else:
                        with contextlib.redirect_stdout(io.StringIO()):stage3_evidence.main()
                        result=json.loads((p/'result.json').read_text())
                        self.assertEqual(policy.validate_evidence(result),'WARN')
                        self.assertTrue(result['failures'])
                        self.assertTrue(result['identical_accepted_current_sections'])

if __name__=='__main__':unittest.main()
