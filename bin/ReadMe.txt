AC3ACM v2.2 by fccHandler
Released July 21, 2012

This is version 2.2 of an AC-3 ACM Codec for Windows. Basically I've
wrapped an Audio Compression Manager interface around the free liba52
library by Aaron Holtzman and Michel Lespinasse, and the ac3enc code
by Fabrice Bellard from ffmpeg.  This makes it possible to preview,
decode, and encode AC-3 audio in VirtualDub (or almost any ACM client).

AC3ACM is released under the GNU General Public License, and you should
have received a copy of this license as "GPL.txt" in the parent folder
of the distribution.  I recommend you read it before using the software.
I make no guarantee that AC3ACM will work for you, and I won't be held
responsible if your computer explodes, and so on...

Note that this zip contains two builds of the codec.  The codec in the
"x86" folder is for use with 32-bit applications, and the codec in the
"x64" folder is for use with 64-bit applications.

If you've never installed the codec before, simply double-click the .bat
file in the same folder with the codec you wish to install (x86 or x64).
You must right-click the .bat file and select "Run as administrator"
if you are using Windows Vista or higher.  If you get a scary warning
that the codec has not passed "Windows Logo testing" (which is
quite true by the way), choose to "Continue Anyway."

The .bat file does not work on older Windows 95 and Windows NT 4
operating systems.  To install the codec on these systems, right-click
the AC3ACM.inf file and select "Install" from the popup menu.

To uninstall the codec, start the Control Panel "Add/Remove Programs"
applet, select "AC-3 ACM Codec 2.2", and press the "Add/Remove" button.

If you have installed a previous version of the codec, I recommend that
you first uninstall the old version before installing this version.

AC3ACM x86 version was compiled with Visual C++ 6.0 and tested on Windows
NT 4, 95 OSR2, 98 SE, and 32-bit Windows XP Pro SP3.  AC3ACM x64 version
was compiled with Visual Studio 2005 but I haven't tested it myself on
64-bit Windows platforms.  You can download the full source code at
http://fcchandler.home.comcast.net.  Future updates of the software
will also be available at that site.

If necessary I can be contacted at fcchandler at comcast dot net, but I
don't check my e-mail very often unless I'm expecting something.  You'll
get a much quicker response by contacting me at the Unofficial VirtualDub
Support Forums.  (As an administrator I visit there several times a day.)
The URL is http://forums.virtualdub.org.

------------------
May the FOURCC be with you...
