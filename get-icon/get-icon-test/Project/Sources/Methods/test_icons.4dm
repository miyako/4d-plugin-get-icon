//%attributes = {"invisible":true}
var $folder; $file; $missing : Object
var $picture : Picture
var $width; $height; $offset; $size; $i : Integer
var $blob : Blob
var $target; $alias; $folderAlias : Object
var $variants : Collection
var $default; $aliasName; $path : Text
ASSERT(Icon No shortcut arrow=1; "Shortcut suppression constant must be Longint 1")
ASSERT(Icon No overlays=2; "All-overlay suppression constant must be Longint 2")
$folder:=Folder(fk database folder)
$file:=File(Folder(fk resources folder).path+"icon-test-日本語.txt")
$file.setText("icon fixture")
$missing:=File($folder.path+"missing-icon-test.txt")

$picture:=Path Get icon($file)
ASSERT(Picture size($picture)>0; "Omitted size must return a picture")
PICTURE TO BLOB($picture; $blob; ".png")
$offset:=16
$width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
$height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
ASSERT(($width=256) & ($height=256); "Omitted size must default to 256")

$picture:=Path Get icon($file; 0)
ASSERT(Picture size($picture)>0; "4D zero/default convention")
For each ($size; [1; 31; 32; 127; 256])
    $picture:=Path Get icon($file; $size)
    ASSERT(Picture size($picture)>0; "File icon must not be empty")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=$size) & ($height=$size); "Exact file PNG dimensions")
    $picture:=Path Get icon($folder; $size)
    ASSERT(Picture size($picture)>0; "Folder icon must not be empty")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=$size) & ($height=$size); "Exact folder PNG dimensions")
End for each

For each ($target; [$file; $folder])
    $aliasName:="icon-test-shortcut-"+Generate UUID
    If (Not(Is macOS))
        $aliasName:=$aliasName+".lnk"
    End if
    $alias:=$target.createAlias(Folder(fk resources folder); $aliasName)
    ASSERT($alias.isAlias; "Shortcut fixture must be a native alias")
    For each ($size; [32; 128; 256])
        $picture:=Path Get icon($alias; $size)
        ASSERT(Picture size($picture)>0; "Default shortcut icon")
        PICTURE TO BLOB($picture; $blob; ".png")
        $default:=Generate digest($blob; SHA256 digest)
        $variants:=New collection
        For each ($i; [0; Icon No shortcut arrow; Icon No overlays; 99])
            $picture:=Path Get icon($alias; $size; $i)
            ASSERT(Picture size($picture)>0; "Explicit shortcut option")
            PICTURE TO BLOB($picture; $blob; ".png")
            $variants.push(Generate digest($blob; SHA256 digest))
            $folderAlias:=Folder($alias.path+"/")
            $picture:=Path Get icon($folderAlias; $size; $i)
            ASSERT(Picture size($picture)>0; "Aliases to files and folders must work through a Folder object")
            PICTURE TO BLOB($picture; $blob; ".png")
            ASSERT(Generate digest($blob; SHA256 digest)=$variants[$variants.length-1]; "Object type must not change the alias icon or overlay policy")
            $offset:=16
            $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
            $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
            ASSERT(($width=$size) & ($height=$size); "Exact alias PNG dimensions")
        End for each
        ASSERT(($variants[0]=$default) & ($variants[3]=$default); "Zero and unknown options preserve default overlays")
        ASSERT($variants[1]=$variants[2]; "Both suppression options remove the shortcut arrow")
        ASSERT($variants[1]#$default; "Default shortcut arrow must change the picture")
        If ($target.isFolder)
            $picture:=Path Get icon($folderAlias; $size)
            ASSERT(Picture size($picture)>0; "Folder alias with omitted overlay option")
            PICTURE TO BLOB($picture; $blob; ".png")
            ASSERT(Generate digest($blob; SHA256 digest)=$default; "Default folder alias arrow must be preserved")
            $picture:=$folderAlias.getIcon($size)
            ASSERT(Picture size($picture)>0; "Native getIcon comparison fixture")
        End if
    End for each
    $alias.delete()
End for each

For each ($i; [0; Icon No shortcut arrow; Icon No overlays; 99])
    $picture:=Path Get icon($file; 128; $i)
    ASSERT(Picture size($picture)>0; "Overlay policies must accept ordinary files")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=128) & ($height=128); "Shortcut flag preserves requested dimensions")
    $picture:=Path Get icon($folder; 0; $i)
    ASSERT(Picture size($picture)>0; "Overlay policies must accept ordinary folders")
    PICTURE TO BLOB($picture; $blob; ".png")
    $offset:=16
    $width:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    $height:=BLOB to longint($blob; Macintosh byte ordering; $offset)
    ASSERT(($width=256) & ($height=256); "Shortcut flag preserves zero/default size")
End for each

$picture:=Path Get icon($folder)
ASSERT(Picture size($picture)>0; "Omitted folder size")
$picture:=Path Get icon($file; -1)
ASSERT(Picture size($picture)=0; "Negative size must return genuinely empty picture")
$picture:=Path Get icon($file; 2147483647)
ASSERT(Picture size($picture)=0; "Overflow size must be empty")
$picture:=Path Get icon($missing)
ASSERT(Picture size($picture)=0; "Missing file must be empty")
$picture:=Path Get icon(Folder($folder.path+"missing-icon-folder/"))
ASSERT(Picture size($picture)=0; "Missing folder must be empty")
$picture:=Path Get icon(Null)
ASSERT(Picture size($picture)=0; "Null object must be empty")
$picture:=Path Get icon(New object)
ASSERT(Picture size($picture)=0; "Object without platformPath must be empty")
$picture:=Path Get icon(New object("platformPath"; 42))
ASSERT(Picture size($picture)=0; "Non-text path must be empty")
For each ($target; [$file; $folder])
    If ($target.isFile)
        $path:=File($target.platformPath; fk platform path).path
    Else
        $path:=Folder($target.platformPath; fk platform path).path
    End if
    If (Substring($path; Length($path); 1)="/")
        $path:=Substring($path; 1; Length($path)-1)
    End if
    For each ($i; [0; Icon No shortcut arrow; Icon No overlays; 99])
        $picture:=Path Get icon($target; 128; $i)
        PICTURE TO BLOB($picture; $blob; ".png")
        $default:=Generate digest($blob; SHA256 digest)
        $picture:=Path Get icon(File($path); 128; $i)
        ASSERT(Picture size($picture)>0; "File object may reference either kind of entry")
        PICTURE TO BLOB($picture; $blob; ".png")
        ASSERT(Generate digest($blob; SHA256 digest)=$default; "File object must preserve entry artwork")
        $picture:=Path Get icon(Folder($path+"/"); 128; $i)
        ASSERT(Picture size($picture)>0; "Folder object may reference either kind of entry")
        PICTURE TO BLOB($picture; $blob; ".png")
        ASSERT(Generate digest($blob; SHA256 digest)=$default; "Folder object must preserve entry artwork")
    End for each
End for each
$picture:=Path Get icon(New object("platformPath"; $file.platformPath))
ASSERT(Picture size($picture)=0; "Path alone is not a filesystem object")
$picture:=Path Get icon(New object("platformPath"; $file.platformPath; "isFile"; True; "isFolder"; True))
ASSERT(Picture size($picture)=0; "Ambiguous object type must be rejected")
$picture:=Path Get icon(New object("platformPath"; $file.platformPath; "isFile"; False; "isFolder"; False))
ASSERT(Picture size($picture)=0; "Object must identify as File or Folder")
$picture:=Path Get icon(New object("platformPath"; $file.platformPath; "isFile"; 1; "isFolder"; False))
ASSERT(Picture size($picture)=0; "Object type markers must be Boolean")
For ($i; 1; 50)
    $picture:=Path Get icon($file; 32)
    ASSERT(Picture size($picture)>0; "Repeated extraction")
End for
$file.delete()
$picture:=Path Get icon($file)
ASSERT(Picture size($picture)=0; "Deleted file must be empty")
