# megagba

INCLUDE=include
INCLUDE_CORE=$(INCLUDE)/gba
INCLUDE_FRONTEND=$(INCLUDE)/frontend
SRC_CORE=gba
SRC_FRONTEND=frontend
DEBUG=debug

CC = gcc
CPPC = g++
CORE_CFLAGS = -O3 -I$(INCLUDE)
FRONT_CFLAGS = -O3 -I$(INCLUDE) -Iimgui `sdl2-config --cflags`
IMGUI_CFLAGS = -O3 -Iimgui

EXE = megagba

BIN_CORE = arm7tdmi.o debugGBA.o gamepak.o gba.o renderer.o
BIN_FRONT = context.o console.o window.o
BIN_IMGUI = imgui.o imgui_tables.o imgui_draw.o imgui_widgets.o imgui_impl_sdlrenderer2.o imgui_impl_sdl2.o

exe: $(EXE)

$(EXE): libmegagba.a libimgui.a libfrontend.a main.o
	$(CPPC) main.o -O3 -L. -lfrontend -limgui -lmegagba `sdl2-config --libs` -o $(EXE)

main.o : main.cpp
	$(CPPC) -c main.cpp -O3 -I$(INCLUDE) -Iimgui

# ----------------------------------------------------------------------
libmegagba: libmegagba.a

libmegagba.a: $(BIN_CORE)
	ar rcs libmegagba.a $(BIN_CORE)

gamepak.o : $(INCLUDE_CORE)/gamepak.h \
              $(SRC_CORE)/gamepak.c
	$(CC) -c $(SRC_CORE)/gamepak.c $(CORE_CFLAGS)

gba.o : $(INCLUDE_CORE)/gba.h $(INCLUDE_CORE)/gamepak.h $(INCLUDE_CORE)/debugGBA.h \
        $(INCLUDE_CORE)/arm7tdmi.h \
       	$(SRC_CORE)/gba.c
	$(CC) -c $(SRC_CORE)/gba.c $(CORE_CFLAGS)

arm7tdmi.o : $(INCLUDE_CORE)/arm7tdmi.h $(INCLUDE_CORE)/gba.h \
        $(SRC_CORE)/arm7tdmi.c
	$(CC) -c $(SRC_CORE)/arm7tdmi.c $(CORE_CFLAGS)

renderer.o : $(INCLUDE_CORE)/renderer.h $(INCLUDE_CORE)/gba.h\
            $(SRC_CORE)/renderer.c
	$(CC) -c $(SRC_CORE)/renderer.c $(CORE_CFLAGS)

debugGBA.o : $(INCLUDE_CORE)/debugGBA.h \
         $(SRC_CORE)/debugGBA.c
	$(CC) -c $(SRC_CORE)/debugGBA.c $(CORE_CFLAGS)

# ---------------------------------------------------------------------

libfrontend: libfrontend.a

libfrontend.a: $(BIN_FRONT)
	ar rcs libfrontend.a $(BIN_FRONT)

context.o : $(INCLUDE_FRONTEND)/context.hpp \
				$(SRC_FRONTEND)/context.cpp
	$(CPPC) -c $(SRC_FRONTEND)/context.cpp $(FRONT_CFLAGS)

console.o : $(INCLUDE_FRONTEND)/console.hpp \
				$(SRC_FRONTEND)/console.cpp
	$(CPPC) -c $(SRC_FRONTEND)/console.cpp $(FRONT_CFLAGS)

window.o : $(INCLUDE_FRONTEND)/window.hpp \
				$(SRC_FRONTEND)/window.cpp
	$(CPPC) -c $(SRC_FRONTEND)/window.cpp $(FRONT_CFLAGS)

# --------------------------------------------------------------------
libimgui: libimgui.a

libimgui.a: $(BIN_IMGUI)
	ar rcs libimgui.a $(BIN_IMGUI)
imgui.o: imgui/imgui.cpp
	$(CPPC) -c imgui/imgui.cpp $(IMGUI_CFLAGS)
imgui_draw.o: imgui/imgui_draw.cpp
	$(CPPC) -c imgui/imgui_draw.cpp $(IMGUI_CFLAGS)
imgui_tables.o: imgui/imgui_tables.cpp
	$(CPPC) -c imgui/imgui_tables.cpp $(IMGUI_CFLAGS)
imgui_widgets.o: imgui/imgui_widgets.cpp
	$(CPPC) -c imgui/imgui_widgets.cpp $(IMGUI_CFLAGS)
imgui_impl_sdlrenderer2.o: imgui/backends/imgui_impl_sdlrenderer2.cpp
	$(CPPC) -c imgui/backends/imgui_impl_sdlrenderer2.cpp $(IMGUI_CFLAGS)
imgui_impl_sdl2.o: imgui/backends/imgui_impl_sdl2.cpp
	$(CPPC) -c imgui/backends/imgui_impl_sdl2.cpp $(IMGUI_CFLAGS)

# --------------------------------------------------------------------
clean:
	rm *.o

