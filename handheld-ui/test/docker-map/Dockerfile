# The map test image (test/run_map_test.sh): the waterfall image + SDL2 (offscreen video, TTF, image) for vibe-hhui's map.
#   docker build --platform linux/arm64 -t vibe-hh-map:trixie handheld-ui/test/docker-map
FROM vibe-hh-wf:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libdrm-dev && rm -rf /var/lib/apt/lists/*
