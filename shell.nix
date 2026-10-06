{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "revc-cmake-dev";

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    gnumake
    gdb
    wayland-scanner
  ];

  buildInputs = with pkgs; [
    openal
    libsndfile
    mpg123
    libGL
    glew

    wayland
    wayland-protocols
    libX11
    libXrandr
    libXinerama
    libXcursor
    libXi
    libXext
    libxkbcommon
  ];

  shellHook = ''
      export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (with pkgs; [
        libGL
        openal
        libsndfile
        mpg123
        glew
        wayland
        libX11
        libXrandr
        libXcursor
        libXi
        libxkbcommon
      ])}:$LD_LIBRARY_PATH"
    '';
}