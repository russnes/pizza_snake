ifeq ($(strip $(PVSNESLIB_HOME)),)
$(error "Please create an environment variable PVSNESLIB_HOME by following this guide: https://github.com/alekmaul/pvsneslib/wiki/Installation")
endif

# list in AUDIOFILES all your .it files in the right order. It will build to generate soundbank file
AUDIOFILES := res/pizza_music.it
# then define the path to generate soundbank data
export SOUNDBANK := res/soundbank
# soundbank conversion flags
SMCONVFLAGS := -s -o $(SOUNDBANK) -V -b 5

include ${PVSNESLIB_HOME}/devkitsnes/snes_rules

.PHONY: bitmaps all musics

#---------------------------------------------------------------------------------
export ROMNAME := pizzasnake

all: bitmaps musics $(ROMNAME).sfc

clean: cleanBuildRes cleanRom cleanGfx cleanAudio

#---------------------------------------------------------------------------------
sprites.bmp sprite_tiles.h: gen_sprites.py
	@echo generating sprite sheet ...
	python3 gen_sprites.py

tiles1.bmp tiles_charmap.h: gen_tiles.py
	@echo generating tileset ...
	python3 gen_tiles.py

trig_table.h: gen_trig.py
	@echo generating trig table ...
	python3 gen_trig.py

sprites.pic: sprites.bmp
	@echo convert bitmap ... $(notdir $@)
	$(GFXCONV) -s 16 -o 16 -u 16 -t bmp -i $<

tiles1.pic: tiles1.bmp
	@echo convert bitmap ... $(notdir $@)
	$(GFXCONV) -s 8 -o 16 -u 16 -t bmp -i $<

splash.pic splash.map splash.pal: splash.bmp
	@echo convert splash bitmap ... $(notdir $@)
	$(GFXCONV) -s 8 -o 16 -u 16 -p -m -R -t bmp -i $<

bitmaps : sprites.pic tiles1.pic splash.pic trig_table.h sprite_tiles.h tiles_charmap.h

musics: $(SOUNDBANK).asm
