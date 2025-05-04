# PatchCry

PatchCry is a kernel-mode driver for Windows 10/11 that implements an enhanced and functional version of the **evade** mechanism from [AdamOron's PatchGuardBypass](https://github.com/AdamOron/PatchGuardBypass). This project is aimed at evading PatchGuard (Kernel Patch Protection) timers by dynamically analyzing KPP timer entries to work around KPP's checks.

> ⚠️ **Disclaimer:** This project is for educational and research purposes only. Unauthorized use of this tool on machines you do not own or have explicit permission to test is strictly prohibited.

---

## 🔧 What’s Updated

PatchCry modernizes the original `evade` code used in [AdamOron's PatchGuardBypass](https://github.com/AdamOron/PatchGuardBypass). It primarily includes:

- Completed the unfinished evade mechanism from the original project.
- Implemented a proper method for retrieving the PRCB (Processor Control Block).
- Verified and adjusted hardcoded offsets for an older Windows 10 build to ensure correct behavior.
- General cleanup of the original code into a working state.

---

## 💡 How It Works

PatchCry works by:

1. Locating the **timer table** and identifying **PatchGuard-related timers** through characteristics like:
   - Specific `DeferredRoutine` addresses.
   - Use of `CcBcbProfiler`, etc.
2. Toggling kernel patches **on and off** dynamically, allowing for PatchGuard-safe modifications.

The evasion context (EVADE_CONTEXT) tracks relevant timers and DPCs, setting custom timers to trigger just before PatchGuard's integrity check runs. This allows the driver to temporarily disable patches during verification and reapply them once the check completes.

---

## ⚠️ Important Notes

- Offsets like KiWaitAlways, HalpClockTimer, PrcbIndex, and others are hardcoded and must be updated manually for your specific Windows version.
- The timer decoding logic in SearchTimerList (especially the pDpc calculation) may vary across Windows builds.
- This project is not universal, use only on systems where the offsets have been verified or updated.

---

## 🗂️ Project Structure

```c
- DriverEntry()         : Initializes globals, starts evasion routine.
- InitializeGlobals()   : Resolves global kernel structure offsets.
- KPPB_Evade()          : Initializes the evasion context and schedules timers to temporarily remove patches before PatchGuard runs, then reapply them afterward.
- FindTimer()           : Locates PatchGuard kernel timers.
- SearchTimerList()     : Walks timer entries to identify targets.
- StartEvasion()        : Disables all patches just before PatchGuard's check.
- StopEvasion()         : Re-enables patches after PatchGuard's check completes.
- EnableAllPatches()    : Applies all intended kernel modifications.
- DisableAllPatches()   : Reverts all modifications before PatchGuard checks run.
```
