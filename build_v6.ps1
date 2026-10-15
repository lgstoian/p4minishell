$env:IDF_PATH = 'D:\esp-idf'
. 'D:\esp-idf\export.ps1' | Out-Null
Remove-Item -Recurse -Force 'D:\Code\new\p4minishell\build' -ErrorAction SilentlyContinue
Remove-Item -Path 'D:\Code\new\p4minishell\sdkconfig' -ErrorAction SilentlyContinue
idf.py -B build -DP4_BOARD=jc1060p470c build 2>&1 | Out-String
