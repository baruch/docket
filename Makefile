all: docketd

docketd: build.ninja dep
	@ninja

build.ninja: configure
	@./configure

.PHONY: dep all
dep:
	@which ninja >/dev/null || (echo "Missing ninja build, on Debian/Ubuntu do: sudo apt-get install ninja-build"; exit 1)
