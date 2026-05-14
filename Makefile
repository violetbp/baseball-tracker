KERRIGAN      := kerrigan
REMOTE_DIR    := ~/baseball-tracker
FIRMWARE_BIN  := firmware/.esphome/build/transit-tracker/.pioenvs/transit-tracker/firmware.factory.bin

RSYNC_EXCLUDE := \
  --exclude='.platformio/' \
  --exclude='firmware/.esphome/' \
  --exclude='.direnv/' \
  --exclude='deps/imgui/' \
  --exclude='simulate/bin/' \
  --exclude='simulate/build/' \
  --exclude='*.zsh_history'

.PHONY: remote remote-sim

remote:
	rsync -az --delete $(RSYNC_EXCLUDE) ./ $(KERRIGAN):$(REMOTE_DIR)/
	ssh $(KERRIGAN) "cd $(REMOTE_DIR) && nix develop --no-update-lock-file --command esphome compile firmware/local-tracker-vars.yaml"
	rsync -az $(KERRIGAN):$(REMOTE_DIR)/$(FIRMWARE_BIN) $(FIRMWARE_BIN)

remote-sim:
	rsync -az --delete $(RSYNC_EXCLUDE) ./ $(KERRIGAN):$(REMOTE_DIR)/
	ssh $(KERRIGAN) "cd $(REMOTE_DIR) && nix develop --no-update-lock-file --command make -C simulate"
