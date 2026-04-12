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

EXE = megagba

BIN_CORE = arm7tdmi.o debugGBA.o gamepak.o gba.o renderer.o
BIN_IMGUI = imgui.o imgui_tables.o imgui_draw.o imgui_widgets.o imgui_impl_sdlrenderer2.o imgui_impl_sdl2.o


exe: $(EXE)

$(EXE): libmegagba.a libimgui.a front_imgui.o main.o
	$(CPPC) main.o front_imgui.o -O3 -L. -lmegagba -limgui `sdl2-config --libs` -o $(EXE)

front_imgui.o : $(INCLUDE_FRONTEND)/front_imgui.hpp \
				$(SRC_FRONTEND)/front_imgui.cpp
	$(CPPC) -c $(SRC_FRONTEND)/front_imgui.cpp -I$(INCLUDE) -Iimgui -O3 `sdl2-config --cflags`

main.o : main.cpp
	$(CPPC) -c main.cpp -I$(INCLUDE) -O3 
# ----------------------------------------------------------------------
core: libmegagba.a

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

# --------------------------------------------------------------------
imgui: libimgui.a

libimgui.a: $(BIN_IMGUI)
	ar rcs libimgui.a $(BIN_IMGUI)
imgui.o: imgui/imgui.cpp
	$(CPPC) -c imgui/imgui.cpp $(CFLAGS) -Iimgui
imgui_draw.o: imgui/imgui_draw.cpp
	$(CPPC) -c imgui/imgui_draw.cpp $(CFLAGS) -Iimgui
imgui_tables.o: imgui/imgui_tables.cpp
	$(CPPC) -c imgui/imgui_tables.cpp $(CFLAGS) -Iimgui
imgui_widgets.o: imgui/imgui_widgets.cpp
	$(CPPC) -c imgui/imgui_widgets.cpp $(CFLAGS) -Iimgui
imgui_impl_sdlrenderer2.o: imgui/backends/imgui_impl_sdlrenderer2.cpp
	$(CPPC) -c imgui/backends/imgui_impl_sdlrenderer2.cpp $(CFLAGS) -Iimgui
imgui_impl_sdl2.o: imgui/backends/imgui_impl_sdl2.cpp
	$(CPPC) -c imgui/backends/imgui_impl_sdl2.cpp $(CFLAGS) -Iimgui

# --------------------------------------------------------------------
clean:
	rm *.o

