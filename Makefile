# Forwards all make targets to the firmware repo, so you can run e.g.
# `make remote-run` from here without cd-ing into ../tracker-yaml.
%:
	@$(MAKE) -C ../tracker-yaml $@
