#pragma once

// A WDI StartupInfo file (#3073), as Windows 11 writes one per sign-in to
// %SystemRoot%\System32\wdi\LogFiles\StartupInfo\<SID>_StartupInfo<N>.xml (there in UTF-16 with a byte
// order mark; UTF-8 here, and test_WindowsStartupImpact.cpp encodes it). Captured from a real sign-in
// and cut down, with the user's name and paths replaced: the structure, element and attribute names,
// time format and units are Windows' own. The sign-in measured:
//   - OneDrive, started by explorer.exe: 13.2 s of CPU, 100.8 MB of disk (High);
//   - Windows Security's tray icon, started by explorer.exe: 26 ms, 204 KB (Low);
//   - Microsoft Edge, started by explorer.exe, with two child processes and a grandchild: 214.7 ms and
//     359 MB itself, 6.2 s of CPU in all (High);
//   - Waves MaxxAudio, started by explorer.exe: 412 ms, 2.1 MB (Medium);
//   - a packaged app (Bing Wallpaper), started by sihost.exe;
//   - an updater that reuses the PID of an Edge process that had exited, with a parent start time of its own,
//     so it is not Edge's child;
//   - a command line whose CDATA holds what looks like markup, which must not be read as such.

#include <string_view>

namespace Platform::StartupInfoTestData
{

inline constexpr std::string_view SIGN_IN_XML = R"xml(<?xml version="1.0" encoding="UTF-16"?>
<StartupData IntervalStartMs="17257" IntervalEndMs="107257">
	<Process Name="C:\Users\Sam\AppData\Local\Microsoft\OneDrive\OneDrive.exe" PID="9120" StartedInTraceSec="18.010">
		<StartTime>2026/10/23:16:52:14.1180331</StartTime>
		<CommandLine><![CDATA["C:\Users\Sam\AppData\Local\Microsoft\OneDrive\OneDrive.exe" /background]]></CommandLine>
		<DiskUsage Units="bytes">100827136</DiskUsage>
		<CpuUsage Units="us">13239718</CpuUsage>
		<ParentPID>10276</ParentPID>
		<ParentStartTime>2026/10/23:16:52:13.8738290</ParentStartTime>
		<ParentName>explorer.exe</ParentName>
	</Process>
	<Process Name="C:\Windows\System32\SecurityHealthSystray.exe" PID="9284" StartedInTraceSec="18.112">
		<StartTime>2026/10/23:16:52:14.2203510</StartTime>
		<CommandLine><![CDATA["C:\Windows\System32\SecurityHealthSystray.exe"]]></CommandLine>
		<DiskUsage Units="bytes">204800</DiskUsage>
		<CpuUsage Units="us">26041</CpuUsage>
		<ParentPID>10276</ParentPID>
		<ParentStartTime>2026/10/23:16:52:13.8738290</ParentStartTime>
		<ParentName>explorer.exe</ParentName>
	</Process>
	<Process Name="C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" PID="11032" StartedInTraceSec="18.640">
		<StartTime>2026/10/23:16:52:14.7480002</StartTime>
		<CommandLine><![CDATA["C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --no-startup-window --win-session-start]]></CommandLine>
		<DiskUsage Units="bytes">359051264</DiskUsage>
		<CpuUsage Units="us">214700</CpuUsage>
		<ParentPID>10276</ParentPID>
		<ParentStartTime>2026/10/23:16:52:13.8738290</ParentStartTime>
		<ParentName>explorer.exe</ParentName>
	</Process>
	<Process Name="C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" PID="11188" StartedInTraceSec="19.020">
		<StartTime>2026/10/23:16:52:15.1290417</StartTime>
		<CommandLine><![CDATA["C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --type=renderer </Process><CpuUsage Units="us">999999999</CpuUsage>]]></CommandLine>
		<DiskUsage Units="bytes">52428800</DiskUsage>
		<CpuUsage Units="us">4100000</CpuUsage>
		<ParentPID>11032</ParentPID>
		<ParentStartTime>2026/10/23:16:52:14.7480002</ParentStartTime>
		<ParentName>msedge.exe</ParentName>
	</Process>
	<Process Name="C:\Program Files (x86)\Microsoft\EdgeWebView\Application\156.0.4314.42\msedgewebview2.exe" PID="11420" StartedInTraceSec="20.300">
		<StartTime>2026/10/23:16:52:16.4013377</StartTime>
		<CommandLine><![CDATA["C:\Program Files (x86)\Microsoft\EdgeWebView\Application\156.0.4314.42\msedgewebview2.exe" --embedded-browser-webview=1]]></CommandLine>
		<DiskUsage Units="bytes">40000000</DiskUsage>
		<CpuUsage Units="us">1800000</CpuUsage>
		<ParentPID>11032</ParentPID>
		<ParentStartTime>2026/10/23:16:52:14.7480002</ParentStartTime>
		<ParentName>msedge.exe</ParentName>
	</Process>
	<Process Name="C:\Windows\System32\conhost.exe" PID="11500" StartedInTraceSec="20.900">
		<StartTime>2026/10/23:16:52:17.0011111</StartTime>
		<CommandLine><![CDATA[\??\C:\Windows\system32\conhost.exe 0xffffffff -ForceV1]]></CommandLine>
		<DiskUsage Units="bytes">8192</DiskUsage>
		<CpuUsage Units="us">90000</CpuUsage>
		<ParentPID>11420</ParentPID>
		<ParentStartTime>2026/10/23:16:52:16.4013377</ParentStartTime>
		<ParentName>msedgewebview2.exe</ParentName>
	</Process>
	<Process Name="C:\Windows\System32\DriverStore\FileRepository\wavesapo14de.inf_amd64_c5e8a53c37f8b1a1\WavesSvc64.exe" PID="9412" StartedInTraceSec="18.300">
		<StartTime>2026/10/23:16:52:14.4002000</StartTime>
		<CommandLine><![CDATA["C:\Windows\System32\DriverStore\FileRepository\wavesapo14de.inf_amd64_c5e8a53c37f8b1a1\WavesSvc64.exe" -Jack]]></CommandLine>
		<DiskUsage Units="bytes">2097152</DiskUsage>
		<CpuUsage Units="us">412000</CpuUsage>
		<ParentPID>10276</ParentPID>
		<ParentStartTime>2026/10/23:16:52:13.8738290</ParentStartTime>
		<ParentName>explorer.exe</ParentName>
	</Process>
	<Process Name="C:\Program Files\WindowsApps\Microsoft.BingWallpaper_1.1.469.0_x86__8wekyb3d8bbwe\BingWallpaper.exe" PID="12040" StartedInTraceSec="25.004">
		<StartTime>2026/10/23:16:52:21.1180000</StartTime>
		<CommandLine><![CDATA["C:\Program Files\WindowsApps\Microsoft.BingWallpaper_1.1.469.0_x86__8wekyb3d8bbwe\BingWallpaper.exe" -ServerName:App.AppX]]></CommandLine>
		<DiskUsage Units="bytes">122340000</DiskUsage>
		<CpuUsage Units="us">4059600</CpuUsage>
		<ParentPID>1312</ParentPID>
		<ParentStartTime>2026/10/23:16:52:01.0050000</ParentStartTime>
		<ParentName>sihost.exe</ParentName>
	</Process>
	<Process Name="C:\Program Files (x86)\Contoso &amp; Co\Updater\updater.exe" PID="11188" StartedInTraceSec="80.500">
		<StartTime>2026/10/23:16:53:16.6000000</StartTime>
		<CommandLine><![CDATA["C:\Program Files (x86)\Contoso & Co\Updater\updater.exe" --wake]]></CommandLine>
		<DiskUsage Units="bytes">0</DiskUsage>
		<CpuUsage Units="us">3969</CpuUsage>
		<ParentPID>1840</ParentPID>
		<ParentStartTime>2026/10/23:16:52:05.2000000</ParentStartTime>
		<ParentName>svchost.exe</ParentName>
	</Process>
	<ReadAheadAnalysisTime>0</ReadAheadAnalysisTime>
	<RurLegacyResourceAttribution>78</RurLegacyResourceAttribution>
</StartupData>
)xml";

/// An older sign-in's file: one process, measured three days before SIGN_IN_XML's.
inline constexpr std::string_view OLDER_SIGN_IN_XML = R"xml(<?xml version="1.0" encoding="UTF-16"?>
<StartupData IntervalStartMs="24308" IntervalEndMs="114308">
	<Process Name="C:\Users\Sam\AppData\Local\Microsoft\OneDrive\OneDrive.exe" PID="7720" StartedInTraceSec="25.120">
		<StartTime>2026/10/20:08:11:02.5000000</StartTime>
		<CommandLine><![CDATA["C:\Users\Sam\AppData\Local\Microsoft\OneDrive\OneDrive.exe" /background]]></CommandLine>
		<DiskUsage Units="bytes">1000</DiskUsage>
		<CpuUsage Units="us">1000</CpuUsage>
		<ParentPID>6600</ParentPID>
		<ParentStartTime>2026/10/20:08:11:01.0000000</ParentStartTime>
		<ParentName>explorer.exe</ParentName>
	</Process>
</StartupData>
)xml";

} // namespace Platform::StartupInfoTestData
