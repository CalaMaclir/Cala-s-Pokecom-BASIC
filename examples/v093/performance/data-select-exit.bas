REM Representative Stage 3 workload; keep source fixed between revisions
DATA 1,2,3,4,"CPB"
S=0
FOR J=1 TO 1000
  RESTORE
  FOR I=1 TO 4
    READ A
    SELECT CASE A
      CASE 1,2
        S=S+A
      CASE 3
        S=S+3
      CASE ELSE
        EXIT FOR
    END SELECT
  NEXT I
  READ B$
NEXT J
PRINT S;B$
END
