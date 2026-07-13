{
  description = "ESPHome dev shell";

  inputs = {
    # nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs.url = "nixpkgs/nixos-26.05";

    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = with pkgs; mkShell {
          buildInputs = [
            esphome
            platformio-core
            python3
            python3Packages.pyserial
            python3Packages.fastapi
            python3Packages.uvicorn
            python3Packages.httpx
            esptool
            SDL2
            SDL2_ttf
            pkg-config
            glib
            nlohmann_json
            curl
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