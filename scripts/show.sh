#!/bin/bash
opt -strip-debug -dot-cfg -analyze $@
dot -Tpdf -o cfg.main.pdf cfg.main.dot
evince cfg.main.pdf
