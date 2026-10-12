# HP thermal SMART feeder

Last updated 2026-10-11.

Feeds real Kingston M.2 SATA SMART temperature into YogaSMC TH0A every10s. Requires smartctl and ioset beside smart-feed.py. Run with --once for a diagnostic capture. Installed as login LaunchAgent org.yjmd2222.YogaSMC.HPSMART; persistent JSONL at ~/Library/Logs/YogaSMC/hp-smart-feed.jsonl. New-kext presence and disk model checked; no SMART/SMC fan writes. Missing acquisition leaves kernel cache to expire30s.

Driver also supplies read-only F0Tg from FTGC and TPCD from PHWZ. Build with existing YogaSMC target; all three keys require reboot after deployment. Main workspace context/acpi.md contains deployment/undo details.
