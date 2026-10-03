param([string]$Script = 'D:\Project\MrProper\tools\ui-smoke.ps1')
$errors = $null
$tokens = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errors)
Write-Host ("[syn] файл: {0}" -f $Script)
Write-Host ("[syn] ошибок разбора: {0}, токенов: {1}" -f $errors.Count, $tokens.Count)
foreach ($e in $errors) { Write-Host ("[syn]   строка {0}: {1}" -f $e.Extent.StartLineNumber, $e.Message) }
if ($errors.Count -eq 0) { Write-Host '[syn] OK' } else { exit 2 }