ws = WScript;
sh = ws.CreateObject("WScript.Shell");
try {
  cygbase = sh.RegRead(
    "HKLM\\Software\\Cygnus Solutions\\Cygwin\\mounts v2\\/\\native"
  );
} catch (e) {
  ws.Echo("Error: Could not read Cygwin registry key.");
  ws.Quit(1);
}
cwd = sh.CurrentDirectory;
link = sh.CreateShortcut("MinTTY.lnk");
link.TargetPath = cwd + "\\mintty.exe";
link.Arguments = "/bin/bash -l";
link.WorkingDirectory = cygbase + "bin";
link.IconLocation = cwd + "\\mintty.exe,0";
link.Description = "Cygwin Terminal";
try {
  link.Save();
} catch (e) {
  ws.Echo("Error: Could not write shortcut to disk.");
  ws.Quit(1);
}
