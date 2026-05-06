{
  description = "ESPHome dev shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          buildInputs = [
            pkgs.esphome
            pkgs.platformio-core
            pkgs.python3
            pkgs.python3Packages.pyserial
            pkgs.python3Packages.fastapi
            pkgs.python3Packages.uvicorn
            pkgs.python3Packages.httpx
            pkgs.esptool
            pkgs.SDL2
            pkgs.SDL2_ttf
            pkgs.pkg-config
            pkgs.glib
            pkgs.nlohmann_json
            pkgs.curl
          ];

          shellHook = ''
            export PLATFORMIO_CORE_DIR=$PWD/.platformio
            export PATH="${pkgs.python3}/bin:$PATH"
            export PKG_CONFIG_PATH="${pkgs.curl.dev}/lib/pkgconfig:${pkgs.SDL2}/lib/pkgconfig:${pkgs.SDL2_ttf}/lib/pkgconfig:${pkgs.glib}/lib/pkgconfig"
            export NIX_SDL2_DEV="${pkgs.SDL2}"
            export NIX_SDL2_TTF="${pkgs.SDL2_ttf}"
            export NIX_NLOHMANN_JSON="${pkgs.nlohmann_json}/include"
          '';
        };
        
      }
      
    );
  
}