Write-Host '--- C: ---'
fsutil fsinfo volumeinfo C:
Write-Host '--- D: ---'
fsutil fsinfo volumeinfo D:
Write-Host '--- partition style D: ---'
fsutil fsinfo ntfsinfo D:
Write-Host '--- sector/cluster D: ---'
fsutil fsinfo sectorinfo D: