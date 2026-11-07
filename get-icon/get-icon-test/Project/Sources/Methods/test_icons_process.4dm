//%attributes = {"invisible":true}
var $1 : Object
var $picture; $native; $small : Picture
var $folder : Object
var $i; $offset; $width; $height : Integer
var $blob : Blob
$folder:=Folder(fk database folder)
For ($i; 1; 100)
    $picture:=Path Get icon($folder; 256)
    $small:=Path Get icon($folder; 31)
    $native:=$folder.getIcon(256)
    ASSERT(Picture size($picture)>0; "Repeated large icon in a cooperative process")
    ASSERT(Picture size($native)>0; "Native icon interleaved with plugin extraction")
    PICTURE TO BLOB($small; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=31) & ($height=31); "Repeated small icon dimensions in a cooperative process")
End for
Use ($1)
    $1.success:=True
    $1.done:=True
End use
