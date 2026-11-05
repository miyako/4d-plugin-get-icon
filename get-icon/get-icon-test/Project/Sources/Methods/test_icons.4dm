//%attributes = {"invisible":true}
var $folder; $file; $missing : Object
var $picture : Picture
var $width; $height; $offset; $size; $i : Integer
var $blob : Blob
var $target; $alias : Object
var $variants : Collection
var $default : Text
ASSERT(Icon No shortcut arrow=1; "Shortcut suppression constant must be Longint 1")
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

For each ($target; [$file; $folder])
    $alias:=$target.createAlias(Folder(fk resources folder); "icon-test-shortcut-"+Generate UUID+".lnk")
    ASSERT($alias.isAlias; "Shortcut fixture must be a native alias")
    For each ($size; [32; 128; 256])
        $picture:=File Get icon($alias; $size)
        ASSERT(Picture size($picture)>0; "Default shortcut icon")
        PICTURE TO BLOB($picture; $blob; ".png")
        $default:=Generate digest($blob; SHA256 digest)
        $variants:=New collection
        For each ($i; [0; Icon No shortcut arrow; 2])
            $picture:=File Get icon($alias; $size; $i)
            ASSERT(Picture size($picture)>0; "Explicit shortcut option")
            PICTURE TO BLOB($picture; $blob; ".png")
            $variants.push(Generate digest($blob; SHA256 digest))
        End for each
        ASSERT(($variants[0]=$default) & ($variants[2]=$default); "Only option 1 suppresses the arrow")
        ASSERT($variants[1]#$default; "Default shortcut arrow must change the picture")
    End for each
    $alias.delete()
End for each

For each ($i; [0; Icon No shortcut arrow; 2])
    $picture:=File Get icon($file; 128; $i)
    ASSERT(Picture size($picture)>0; "Optional shortcut flag must accept ordinary files")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=128) & ($height=128); "Shortcut flag preserves requested dimensions")
    $picture:=Folder Get icon($folder; 0; $i)
    ASSERT(Picture size($picture)>0; "Optional shortcut flag must accept ordinary folders")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=256) & ($height=256); "Shortcut flag preserves zero/default size")
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
$picture:=File Get icon(New object)
ASSERT(Picture size($picture)=0; "Object without platformPath must be empty")
$picture:=File Get icon(New object("platformPath"; 42))
ASSERT(Picture size($picture)=0; "Non-text path must be empty")
$picture:=File Get icon($folder)
ASSERT(Picture size($picture)=0; "Folder passed as file must be empty")
$picture:=Folder Get icon($file)
ASSERT(Picture size($picture)=0; "File passed as folder must be empty")
For ($i; 1; 50)
    $picture:=File Get icon($file; 32)
    ASSERT(Picture size($picture)>0; "Repeated extraction")
End for
$file.delete()
$picture:=File Get icon($file)
ASSERT(Picture size($picture)=0; "Deleted file must be empty")
