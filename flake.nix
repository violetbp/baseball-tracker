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
            pkgs.python3Packages.fastapi # some of these are for the replay server
            pkgs.python3Packages.uvicorn
            pkgs.python3Packages.httpx
            pkgs.esptool
          ];

          shellHook = ''
            export PLATFORMIO_CORE_DIR=$PWD/.platformio
            export PATH="${pkgs.python3}/bin:$PATH"
          '';
        };
        
      }
      
    );
  
}