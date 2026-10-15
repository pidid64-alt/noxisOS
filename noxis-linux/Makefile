# NoxisOS Cachy Edition — convenience targets (the real work is in build.sh)
SHELL := /usr/bin/env bash

PROFILE := configs/noxis
OUT     := out
WORK    := work

.PHONY: help all iso deps verify assets tier-v3 tier-v4 assemble hyprcheck clean distclean run shellcheck

help:
	@sed -n '2,14p' build.sh
	@echo
	@echo "make targets:"
	@echo "  make iso        build the ISO into $(OUT)/"
	@echo "  make deps       install build dependencies"
	@echo "  make verify     check that every package exists in the repos"
	@echo "  make assets     regenerate branding PNGs from assets/ (needs Pillow)"
	@echo "  make tier-v3    build with x86-64-v3 optimized base packages"
	@echo "  make hyprcheck  verify the shipped Hyprland config with Hyprland's parser"
	@echo "  make assemble   join+verify ISO parts downloaded from a release"
	@echo "                  (parts='DIR', out='FILE'; default: . and detected name)"
	@echo "  make clean      remove the ISO output"
	@echo "  make distclean  also remove the mkarchiso work directory"

all: iso

iso:
	./build.sh

deps:
	./build.sh --deps

verify:
	./scripts/check-packages.sh $(PROFILE)

assets:
	python3 scripts/make-branding-assets.py

tier-v3:
	./build.sh --tier v3

tier-v4:
	./build.sh --tier v4

# Join the parts of a downloaded release and verify them:
#   make assemble parts=~/Downloads
assemble:
	./scripts/assemble-iso.sh --parts '$(parts)' $(if $(out),--out '$(out)',)

hyprcheck:
	./scripts/check-hyprland-config.sh $(PROFILE)

clean:
	rm -rf -- $(OUT)

distclean: clean
	rm -rf -- $(WORK)
