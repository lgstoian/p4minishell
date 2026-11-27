@echo off
rem ENGINE3D.BAT - numeric, batch-friendly exercise of every gfx3d verb.
rem No model file, no parser: the mesh is a bounded numeric vertex/face list.
echo [M-ENGINE3D]
gfx init 64 64
gfx3d init
gfx3d camera 0 0 -8 0 0 0 0 1 0
gfx3d project 90 1 1 1 100
gfx3d light 0 0 1 256
gfx3d mesh quad 12 6 -2 -2 -4 2 -2 -4 0 2 -4 -2 -2 0 2 -2 0 0 2 0 2 0 1 2 3 4 5
gfx3d mode solid cull
gfx3d rotate y 30
gfx3d translate 0 0 0
gfx3d scale 1 1 1
gfx3d identity
gfx3d draw quad
gfx3d clear 0
gfx3d stats
gfx3d status
echo [M-ENGINE3D-DONE]
gfx3d close
gfx close
