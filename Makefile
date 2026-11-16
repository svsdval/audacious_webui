# Сборка плагина Web Remote для Audacious.
#
#   make            собрать webremote.so
#   sudo make install   установить в каталог плагинов Audacious
#   make sandbox    запустить отдельный тестовый Audacious с плагином (без sudo)
#
# Если пакет audacious-dev установлен, заголовки берутся через pkg-config.
# Иначе скачиваются исходники той же версии, что и установленный Audacious,
# и из них используются только заголовки libaudcore.

CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pthread

BUILD := build
PLUGIN := $(BUILD)/webremote.so

ifeq ($(shell pkg-config --exists audacious && echo yes),yes)
  AUD_CFLAGS := $(shell pkg-config --cflags audacious)
  AUD_LIBS   := $(shell pkg-config --libs audacious)
  PLUGIN_DIR ?= $(shell pkg-config --variable=plugin_dir audacious)
  HEADERS    :=
else
  AUD_VERSION ?= $(shell audacious --version 2>/dev/null | sed -n 's/^Audacious \([0-9.]*\).*/\1/p')
  AUD_LIB     ?= $(firstword $(wildcard /usr/lib/*/libaudcore.so.5 /usr/lib64/libaudcore.so.5 /usr/lib/libaudcore.so.5 /usr/local/lib/libaudcore.so.5))
  AUD_SRC     := third_party/audacious-$(AUD_VERSION)
  HEADERS     := $(AUD_SRC)/src/libaudcore/audio.h
  AUD_CFLAGS  := -I$(AUD_SRC)/src
  AUD_LIBS    := $(AUD_LIB)
  PLUGIN_DIR  ?= $(dir $(AUD_LIB))audacious
endif

all: $(PLUGIN)

$(PLUGIN): src/webremote.cc $(BUILD)/index_html.inc $(HEADERS)
	$(CXX) $(CXXFLAGS) $(AUD_CFLAGS) -I$(BUILD) -shared -o $@ $< $(AUD_LIBS)

# веб-страница встраивается в плагин как строковый литерал
$(BUILD)/index_html.inc: web/index.html
	@mkdir -p $(BUILD)
	{ printf 'R"AUDWEBRAW('; cat $<; printf ')AUDWEBRAW"\n'; } > $@

$(HEADERS):
	@test -n "$(AUD_VERSION)" || { echo "Не удалось определить версию Audacious; задайте AUD_VERSION=x.y"; exit 1; }
	@test -n "$(AUD_LIB)" || { echo "Не найдена libaudcore.so.5; задайте AUD_LIB=/путь/libaudcore.so.5"; exit 1; }
	mkdir -p third_party
	curl -fsSL -o third_party/audacious-$(AUD_VERSION).tar.bz2 \
	    https://distfiles.audacious-media-player.org/audacious-$(AUD_VERSION).tar.bz2
	tar -xf third_party/audacious-$(AUD_VERSION).tar.bz2 -C third_party audacious-$(AUD_VERSION)/src/libaudcore
	sed 's/@BIGENDIAN@/(__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)/' \
	    $(AUD_SRC)/src/libaudcore/audio.h.in > $@

install: $(PLUGIN)
	install -D -m 644 $(PLUGIN) $(DESTDIR)$(PLUGIN_DIR)/General/webremote.so

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_DIR)/General/webremote.so

# Отдельный экземпляр Audacious со своими настройками и своим каталогом
# плагинов: позволяет попробовать плагин без установки в систему.
sandbox: $(PLUGIN)
	./tools/sandbox.sh $(PLUGIN) $(PLUGIN_DIR)

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall sandbox clean
