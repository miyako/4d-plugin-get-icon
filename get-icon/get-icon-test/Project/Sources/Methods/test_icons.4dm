//%attributes = {"invisible":true}
var $folder; $file; $missing : Object
var $picture : Picture
var $width; $height; $offset; $size; $i : Integer
var $blob : Blob
$folder:=Folder(fk database folder)
$file:=File(Folder(fk resources folder).path+"icon-test-日本語.txt")
$file.setText("icon fixture")
$missing:=File($folder.path+"missing-icon-test.txt")

$picture:=File Get icon($file)
ASSERT(Picture size($picture)>0; "Omitted size must return a picture")
PICTURE TO BLOB($picture; $blob; ".png")
$offset:=16
$width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
$height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
ASSERT(($width=256) & ($height=256); "Omitted size must default to 256")

$picture:=File Get icon($file; 0)
ASSERT(Picture size($picture)>0; "4D zero/default convention")
For each ($size; [1; 31; 32; 127; 256])
    $picture:=File Get icon($file; $size)
    ASSERT(Picture size($picture)>0; "File icon must not be empty")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=$size) & ($height=$size); "Exact file PNG dimensions")
    $picture:=Folder Get icon($folder; $size)
    ASSERT(Picture size($picture)>0; "Folder icon must not be empty")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=$size) & ($height=$size); "Exact folder PNG dimensions")
End for each

$picture:=Folder Get icon($folder)
ASSERT(Picture size($picture)>0; "Omitted folder size")
$picture:=File Get icon($file; -1)
ASSERT(Picture size($picture)=0; "Negative size must return genuinely empty picture")
$picture:=File Get icon($file; 2147483647)
ASSERT(Picture size($picture)=0; "Overflow size must be empty")
$picture:=File Get icon($missing)
ASSERT(Picture size($picture)=0; "Missing file must be empty")
$picture:=Folder Get icon(Folder($folder.path+"missing-icon-folder/"))
ASSERT(Picture size($picture)=0; "Missing folder must be empty")
$picture:=File Get icon(Null)
ASSERT(Picture size($picture)=0; "Null object must be empty")
For ($i; 1; 50)
    $picture:=File Get icon($file; 32)
    ASSERT(Picture size($picture)>0; "Repeated extraction")
End for
$file.delete()
$picture:=File Get icon($file)
ASSERT(Picture size($picture)=0; "Deleted file must be empty")
