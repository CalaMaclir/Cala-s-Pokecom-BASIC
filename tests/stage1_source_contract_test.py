from pathlib import Path
r=Path(__file__).resolve().parents[1]
editor=(r/'src/core/full_screen_editor.cpp').read_text()
assert 'SPLIT LINE - MANUAL NUMBER' in editor
assert 'INSERT LINE BEFORE - MANUAL NUMBER' in editor
assert 'model_.cursor() == 0' in editor
assert 'USE F5 INSERT LINE TO ADD A LOGICAL LINE' not in editor
store=(r/'src/core/program_store.cpp').read_text()
assert 'replace_line_pair' in store and 'second_edit' in store
ui=(r/'src/core/repl.cpp').read_text()
assert 'collect_directory_entries' in ui and 'pick_path_file' in ui
assert 'format_status_filename' in ui and 'format_quick_filename' in ui
assert 'file_paths::basename(path)' in ui
assert 'chdir(' not in ui
assert 'DIRECTORY RENAME NOT AVAILABLE' not in ui
transfer=(r/'src/core/transfer_file.cpp').read_text()
assert 'receive ? SafeFileWriter::valid_root_name(name)' in transfer
print('Stage 1 source contracts: PASS')
