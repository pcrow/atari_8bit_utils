   R��A�B�C�SOURC�BYTE�ADD�BYT� �    � ��  x ��     @g     @"     A$    A��   
 �;A0   , �;A    , �;@2    ,d @�    @    +      n ''(DISK OPERATING SYSTEM   9/24/79x (COPYRIGHT 1979 ATARI(� **("B. RUN CARTRIDGE  N. DEFINE DEVICE� (	(((� (SELECT ITEM� @    �� �4B� &�0N(NO SUCH ITEM&
AP   �((GOOD CHOICE!(�F%(Device Definer by Preson CrowF(Loosely based on DOS 1 code.�O()(This will create a binary loadO(!file that will redirect I/O for a�B'(given device (e.g. C: or P:) toB(a file of your choice.�L&(Any open of the device will beL(!redirected to the specified file.,,($The file can be on any device, like:(  D2:PRINT.LOG(  H:devices/printer.txt$(  N:....(L''(DEVICE LETTER TO REDIRECT FROM?VK@    �+B:�,@    )�2A)�3ZA(INVALID SELECTIONK
A    `6�-@:�,�,,(TARGET FILE FOR ALL I/O TO >:�,:�@    ��sNB:�, @    )+�7@    <@    ,0:*�7@    <@    ,0:,)�2A)�3Zf(INVALID TARGET FILEi(s
A    ++(#NAME OF BINARY LOAD FILE TO CREATE?@    �(3B:�, @    ((INVALID FILE NAME3(G.1300x6�-@    %B:�,�1(1(The binary load file needs � bytes�**("of memory to save the redirection.�Q� @7    5(!The init routine at the same baseQ(address needs 37 bytes.�/(/($The default location is the 128-byte�!!(cassette buffer at $0400.�,(,(!WHERE TO LOCATE REDIRECTION CODE?�@    ��
�4 6�.$0400� FIXME: CONVERT C$ TO ADDR��8)�7@    <@    ,4$*B:�,!@    86�.�7@    ,�6�-      �W"�10*�7@    <@    ,/9M6�-�$@    %@:�7@    <@    ,,&@:0,W
A`   _"�1A*�7@    <@    ,/FU6�-�$@    %@:�7@    <@    ,,&@:A,%@    _
A`   /%(INVALID CHARACTERS IN ADDRESS/
A@   +B:�,!@    !6�.�7@    ,+
A0   @W� AV   /(SORRY, DAVE,I CAN'T DO THATM(MUST NOT SART ON ZER PAGEW
A@   J]�@R    $A$   8(SORRY, DAVE, I CAN'T DO THATS(MUST START BELOW $D000]
A@   � ����Š��Ԡ���Š�Ϡ¤��  @    @          ��8"��      *� AV   .(@    >:�,8
A     � ��������Ġ�Ơ���ś��"A��   
A!    � �����������Š�Ϡ�������ś�*�"Aff    (@    >:�,*
A     ## ��������������ĺ���ҫ��̭�����!!� A    )�A     A `   P6�-�%�&A    F(@    >:�&P:�'AV   ,$AV   ,>:�'AV   ,P
A     "" ��������������Ƞ���Š�Ơ���ěL�A     *� A0    )6�-�%�&A     B(@    >:�'AV   ,L
A     !! �������������נ���Š�Ơ���ěY�A0    *� A@    )6�-�%�&A0    O(@    >:�&P:�'AV   ,$AV   ,Y
A      ��������������Ҡ���ī��� � A@    )�AP    &%U6�-�%�&A@    %B:�,K(@    >:�&P:�'AV   ,$AV   ,>:�'AV   ,U
A     4(@    �>:      ,> @    (
FILE READY '%% DATA FOR INITIALIZATION ROUTINE�'255,255,1000,1036�$'ll169,6666,168,162,30,221,26,3,240,12,189,26,3,240,7,152,202,202,202,16,240,96,152,157,26,3,169,3000,157�.'27,3,169,2000,157,28,3,96�8'226,2,227,2,1000� N** DATA FOR PERSISTENT REDIRECTION CODE�*N1000,4015�4N881001,169,3015,157,68,3,169,2015,157,69,3,76,86,228�0u

9999� �
H:RDIR.BAS