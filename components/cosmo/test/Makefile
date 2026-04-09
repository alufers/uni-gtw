.PHONY: build run clean

build:
	cmake -B build -S . && cmake --build build

run: build
	./build/test_cosmo

clean:
	rm -rf build
