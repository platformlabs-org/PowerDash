========================================================================
    PowerDashSYS Project Overview
========================================================================

Issue 1: Cannot open 'trace.h' (CreateFile error 2)
Cause: WPP tracing is enabled but the file is missing
Fix: Disable "Run WPP Tracing" in project settings

------------------------------------------------------------------------

Issue 2: Unresolved external symbols
- WdmlibIoCreateDeviceSecure
- SDDL_DEVOBJ_SYS_ALL_ADM_ALL

Cause: Missing required library

Fix:
1. Linker > General > Additional Library Directories:
   C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\x64
2. Linker > Input > Additional Dependencies:
   wdmsec.lib
