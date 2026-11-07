//%attributes = {"invisible":true}
var $1 : Object
var $picture : Picture
$picture:=Path Get icon(Folder(fk database folder); 31)
Use ($1)
    $1.success:=Picture size($picture)>0
    $1.done:=True
End use
