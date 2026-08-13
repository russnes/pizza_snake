ifeq ($(strip $(PVSNESLIB_HOME)),)
$(error "Please create an environment variable PVSNESLIB_HOME by following this guide: https://github.com/alekmaul/pvsneslib/wiki/Installation")
endif

include ${PVSNESLIB_HOME}/devkitsnes/snes_rules

.PHONY: bitmaps all

#---------------------------------------------------------------------------------
export ROMNAME := pizzasnake

all: bitmaps $(ROMNAME).sfc

clean: cleanBuildRes cleanRom cleanGfx

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

bitmaps : sprites.pic tiles1.pic trig_table.h sprite_tiles.h tiles_charmap.h
