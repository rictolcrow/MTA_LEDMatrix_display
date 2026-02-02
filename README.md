# MTA_LEDMatrix_display
led matrix display of NYC MTA trains on ESP32.

Uses an Adafruit Matrix Portal S3 board and a 64x64 led matrix display (Version: IS-P2.5-32S-64x64-SMD2020-4x45⁰).

There is a standalone Python script to get the MTA information that I used to get going.

This currently only decodes the NYC MTA A trains going uptown from the 145th St. station.

I have a font (nycta_r464pt7b.h) that should go in the Adafruit GFX library's Font folder. I edited from the original somewhere and it seems to work nicely with the matrix.

https://rop.nl/truetype2gfx/

https://www.dafont.com/bitmap.php?text=NEXT+ARRIVAL

https://gfxfont.netlify.app/#glyph73

https://rgbcolorpicker.com/565

https://www.pixilart.com/draw/matrix-led-effect-fc161733e85470e#

https://github.com/kudp02/wled-matrix-tool/tree/main

https://snowb.org/

https://fontstruct.com/fontstructions/download/492696

Intended to let my goofy son know when he has to leave to get to school on time. 
