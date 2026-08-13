.include "hdr.asm"
.section ".rodata1" superfree
gfxsprites: .incbin "sprites.pic"
gfxsprites_end:
palsprites: .incbin "sprites.pal"
palsprites_end:
gfxwalls: .incbin "tiles1.pic"
gfxwalls_end:
palwalls: .incbin "tiles1.pal"
palwalls_end:
.ends
