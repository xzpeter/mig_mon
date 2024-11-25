.PHONY: default clean cscope

default:
	@meson setup build -Doptimization=3 -Ddebug=true -Dwarning_level=3 -Dwerror=true
	@cd build && meson compile

cscope:
	@cscope -bq *.c

clean:
	@rm -rf build/ cscope*
