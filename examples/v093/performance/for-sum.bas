REM 5000*5001/2=12502500 stays below float32 exact integer limit
S=0
FOR I=1 TO 5000
  S=S+I
NEXT I
PRINT S
END
