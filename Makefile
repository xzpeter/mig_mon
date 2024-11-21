.PHONY: default clean cscope

default:
	# optimization=3  => -O3
	# debug=true      => -g
	# warning_level=3 => -Wall
	# werror=true     => -Werror
	@meson setup build -Doptimization=3 -Ddebug=true -Dwarning_level=3 -Dwerror=true
	@cd build && meson compile

cscope:
	@cscope -bq *.c

clean:
	@rm -rf build/ cscope*
