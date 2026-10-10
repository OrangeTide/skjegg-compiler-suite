# Build the Skjegg manual: render every doc/ Markdown file to a standalone
# HTML page (and optionally a PDF) with pandoc, and generate the landing page.
#
# This is self-contained and separate from the top-level build.  The source of
# truth is the Markdown under tools/, languages/, and internals/; adding a new
# doc/<category>/<name>.md adds a page with no edit here.
#
#   make -C doc            build the HTML site into build/manual (default)
#   make -C doc pdf        also render per-page PDFs
#   make -C doc all        HTML + PDF
#   make -C doc clean      remove the built site
#
# The built site uses only relative links, so build/manual/index.html opens
# directly in a browser over file:// for a local preview.
#
# Requires pandoc.  PDF output additionally requires typst.

PANDOC := pandoc
TYPST  := typst

ROOT := ..
OUT  := $(ROOT)/build/manual

TEMPLATE := article-template.html
FILTER   := link-rewrite.lua
INDEXGEN := gen-index.sh
CSS      := article.css

# Every manual page, relative to this directory.
MD   := $(shell find tools languages internals -name '*.md' | sort)
HTML := $(patsubst %.md,$(OUT)/%.html,$(MD))
PDF  := $(patsubst %.md,$(OUT)/%.pdf,$(MD))
MDCP := $(patsubst %.md,$(OUT)/%.md,$(MD))

PANDOC_HTML := --template=$(TEMPLATE) --lua-filter=$(FILTER) \
	--toc --standalone --from=gfm --to=html5

PANDOC_PDF := --pdf-engine=$(TYPST) --toc --number-sections --from=gfm \
	-V mainfont="TeX Gyre Pagella" -V monofont="TeX Gyre Cursor" \
	-V fontsize=10pt

.PHONY: site html pdf all clean
site: html $(MDCP) $(OUT)/index.html $(OUT)/$(CSS)
html: $(HTML)
pdf: $(PDF)
all: html pdf $(MDCP) $(OUT)/index.html $(OUT)/$(CSS)

# One HTML page.  The title is the document's first level-1 heading, which is
# then stripped from the body so it is not repeated under the page header.  The
# category label and the "../" depth prefix are derived from the path.
$(OUT)/%.html: %.md $(TEMPLATE) $(FILTER)
	@mkdir -p $(dir $@)
	@title=$$(sed -n 's/^# //p' $< | head -1 | tr -d '`'); \
	 prefix=$$(printf '%s' '$*' | awk -F/ '{s="";for(i=1;i<NF;i++)s=s"../";printf "%s",s}'); \
	 cat=$$(printf '%s' '$*' | cut -d/ -f1); \
	 case "$$cat" in \
	   tools) label=Tools;; \
	   internals) label=Internals;; \
	   languages) label=Languages;; \
	   *) label=Documentation;; \
	 esac; \
	 case '$*' in languages/kobold/*) label="Kobold-Pascal";; esac; \
	 base=$$(basename '$*'); \
	 sed '1{/^# /d;}' $< | \
	   SITE_PREFIX="$$prefix" $(PANDOC) $(PANDOC_HTML) \
	     -M title="$$title" \
	     -V css-path="$$prefix" -V index-path="$$prefix" \
	     -V category="$$label" \
	     -V md-file="$$base.md" -V pdf-file="$$base.pdf" \
	     -o $@ -
	@echo "  HTML  $@"

# One PDF page.
$(OUT)/%.pdf: %.md
	@mkdir -p $(dir $@)
	@title=$$(sed -n 's/^# //p' $< | head -1 | tr -d '`'); \
	 sed '1{/^# /d;}' $< | \
	   $(PANDOC) $(PANDOC_PDF) -M title="$$title" -o $@ -
	@echo "  PDF   $@"

# A copy of the source Markdown beside its page, for the "Markdown" link.
$(OUT)/%.md: %.md
	@mkdir -p $(dir $@)
	@cp $< $@

$(OUT)/index.html: $(INDEXGEN) $(MD)
	@mkdir -p $(OUT)
	@sh $(INDEXGEN) > $@
	@echo "  INDEX $@"

$(OUT)/$(CSS): $(CSS)
	@mkdir -p $(OUT)
	@cp $< $@

clean:
	rm -rf $(OUT)
