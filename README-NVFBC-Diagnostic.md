# NvFBC driver diagnostic for FG V15 Clean

Standalone read-only x64 executable. It does not initialize a capture session,
modify registry values, install drivers, or change V15.

1. Extract the ZIP.
2. Run NVFBC-Diagnostic.exe.
3. The program displays the current NvFBC DLL status, whether the
   keylase wrapper backup exists, the NVFBCEnable registry value,
   and NvFBC_GetStatusEx results.
4. Send NvFBC-Diagnostic.txt from the same folder.

Only inspect the diagnostic values and do not upload system DLLs.
