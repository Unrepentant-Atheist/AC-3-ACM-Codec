# Microsoft Developer Studio Project File - Name="AC3ACM" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Dynamic-Link Library" 0x0102

CFG=AC3ACM - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "AC3ACM.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "AC3ACM.mak" CFG="AC3ACM - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "AC3ACM - Win32 Release" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE "AC3ACM - Win32 Debug" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""
CPP=cl.exe
MTL=midl.exe
RSC=rc.exe

!IF  "$(CFG)" == "AC3ACM - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 1
# PROP Target_Dir ""
# ADD BASE CPP /nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "AC3ACM_EXPORTS" /YX /FD /c
# ADD CPP /nologo /MT /W3 /GX /O2 /Ob2 /I "..\a52dec-0.7.5-cvs\liba52" /I "..\a52dec-0.7.5-cvs\include" /I "..\a52dec-0.7.5-cvs\vc++" /I "..\src\ac3enc" /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "AC3ACM_EXPORTS" /D __restrict=restrict /FD /c
# SUBTRACT CPP /YX
# ADD BASE MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /dll /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib winmm.lib msacm32.lib /nologo /dll /pdb:none /machine:I386 /out:"..\bin\x86\AC3ACM.acm"
# SUBTRACT LINK32 /nodefaultlib

!ELSEIF  "$(CFG)" == "AC3ACM - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Ignore_Export_Lib 1
# PROP Target_Dir ""
# ADD BASE CPP /nologo /MTd /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "AC3ACM_EXPORTS" /YX /FD /GZ /c
# ADD CPP /nologo /MTd /W3 /GX /Zi /Od /I "..\a52dec-0.7.5-cvs\liba52" /I "..\a52dec-0.7.5-cvs\include" /I "..\a52dec-0.7.5-cvs\vc++" /I "..\src\ac3enc" /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /D "_USRDLL" /D "AC3ACM_EXPORTS" /D __restrict=restrict /FD /GZ /c
# SUBTRACT CPP /YX
# ADD BASE MTL /nologo /D "_DEBUG" /mktyplib203 /win32
# ADD MTL /nologo /D "_DEBUG" /mktyplib203 /win32
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /dll /debug /machine:I386 /pdbtype:sept
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib winmm.lib msacm32.lib /nologo /dll /incremental:no /debug /machine:I386 /out:"E:/WINDOWS/System32/AC3ACM.acm" /pdbtype:sept
# SUBTRACT LINK32 /pdb:none

!ENDIF 

# Begin Target

# Name "AC3ACM - Win32 Release"
# Name "AC3ACM - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE=..\src\AC3ACM.cpp
# End Source File
# Begin Source File

SOURCE=..\src\AC3ACM.def
# End Source File
# Begin Source File

SOURCE=..\src\AC3ASM.asm

!IF  "$(CFG)" == "AC3ACM - Win32 Release"

# PROP Ignore_Default_Tool 1
# Begin Custom Build - Assembling $(InputPath)...
IntDir=.\Release
InputPath=..\src\AC3ASM.asm
InputName=AC3ASM

"$(IntDir)\$(InputName).obj" : $(SOURCE) "$(INTDIR)" "$(OUTDIR)"
	yasm -X vc -f win32 -o "$(IntDir)\$(InputName).obj" "$(InputPath)"

# End Custom Build

!ELSEIF  "$(CFG)" == "AC3ACM - Win32 Debug"

# PROP Ignore_Default_Tool 1
# Begin Custom Build - Assembling $(InputPath)...
IntDir=.\Debug
InputPath=..\src\AC3ASM.asm
InputName=AC3ASM

"$(IntDir)\$(InputName).obj" : $(SOURCE) "$(INTDIR)" "$(OUTDIR)"
	yasm -X vc -g vc8 -f win32 -o "$(IntDir)\$(InputName).obj" "$(InputPath)"

# End Custom Build

!ENDIF 

# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE=..\src\WinDDK.h
# End Source File
# End Group
# Begin Group "Resource Files"

# PROP Default_Filter "ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe"
# Begin Source File

SOURCE=..\src\AC3ACM.rc
# End Source File
# Begin Source File

SOURCE=..\src\resource.h
# End Source File
# End Group
# Begin Group "liba52"

# PROP Default_Filter ""
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\include\a52.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\a52_internal.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\bit_allocate.c"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\bitstream.c"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\bitstream.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\vc++\config.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\downmix.c"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\imdct.c"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\vc++\inttypes.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\include\mm_accel.h"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\parse.c"
# End Source File
# Begin Source File

SOURCE="..\a52dec-0.7.5-cvs\liba52\tables.h"
# End Source File
# End Group
# Begin Group "ac3enc"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\src\ac3enc\ac3enc.cpp
# End Source File
# Begin Source File

SOURCE=..\src\ac3enc\ac3enc.h
# End Source File
# Begin Source File

SOURCE=..\src\ac3enc\ac3tab.h
# End Source File
# Begin Source File

SOURCE=..\src\ac3enc\common.h
# End Source File
# End Group
# End Target
# End Project
