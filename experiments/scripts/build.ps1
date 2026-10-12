$ErrorActionPreference='Stop'
$llvm='D:\llvm\clang+llvm-19.1.7-x86_64-pc-windows-msvc\bin'
$wdk='D:\work\nuget\Microsoft.Windows.WDK.x64\c'
$sdk='D:\work\nuget\Microsoft.Windows.SDK.CPP\c'
$sdkx='D:\work\nuget\Microsoft.Windows.SDK.CPP.x64\c'
$ver='10.0.26100.0'
$inc=@("$wdk\Include\$ver\km","$wdk\Include\$ver\km\crt","$wdk\Include\$ver\shared","$wdk\Include\wdf\kmdf\1.15","$sdk\Include\$ver\shared") 
$src=$args[0]; $name=$args[1]
$o="D:\jsl-audio\out"; New-Item -ItemType Directory -Force $o | Out-Null
$cl=@('/c','/kernel','/GS-','/Gy','/Zi','/O1','/W3','-Wno-everything','-fms-extensions','-m64','/D_AMD64_','/DAMD64','/D_WIN64','/DNDEBUG','/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00','/DKMDF_VERSION_MAJOR=1','/DKMDF_VERSION_MINOR=15','/DPOOL_NX_OPTIN=1')
foreach($i in $inc){ $cl += "-imsvc"; $cl += $i }
& "$llvm\clang-cl.exe" @cl $src "/Fo$o\$name.obj"
if($LASTEXITCODE){ throw 'compile failed' }
$libs=@("$wdk\Lib\$ver\km\x64","$wdk\Lib\wdf\kmdf\x64\1.15")
$ln=@('/DRIVER','/SUBSYSTEM:NATIVE','/ENTRY:FxDriverEntry','/NODEFAULTLIB','/RELEASE','/NXCOMPAT','/DYNAMICBASE','/MERGE:_TEXT=.text','/MERGE:.retplne=.rdata','/MERGE:_guard_c=.rdata','/MERGE:_guard_d=.rdata','/MERGE:memset_=.text','/GUARD:NO','/HIGHENTROPYVA:NO','/TSAWARE:NO','/BASE:0x10000','/INTEGRITYCHECK','/OPT:REF','/MERGE:_PAGE=PAGE','/DEBUG','/MACHINE:X64',"/OUT:$o\$name.sys","$o\$name.obj",'ntoskrnl.lib','hal.lib','wmilib.lib','ntstrsafe.lib','wdfldr.lib','wdfdriverentry.lib','BufferOverflowFastFailK.lib')
foreach($l in $libs){ $ln += "/LIBPATH:$l" }
& "$llvm\lld-link.exe" @ln
if($LASTEXITCODE){ throw 'link failed' }
Get-Item "$o\$name.sys" | select Name,Length


