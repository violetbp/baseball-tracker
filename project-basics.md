this is an esphome project to track a baseball game based around working alongside a prebuilt project that tracks something else 


the firmware YAML and build now live in the sibling `../tracker-yaml` repo. run this project by `cd ../tracker-yaml`, entering the dev environemnt with `nix develop`, and then running `esphome run firmware/local-tracker-vars.yaml --device /dev/ttyACM0` to upload to the device and start logging



