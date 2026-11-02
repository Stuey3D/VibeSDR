#!/bin/sh
# fetch.sh — the downloads gen-sea-areas.mjs reads (NOT committed; the processed polygons are: sea-areas.cache.json).
#   sh scripts/mapdata/sea-areas/fetch.sh <empty dir>   then   node scripts/gen-sea-areas.mjs --src <that dir>
# Licences: scripts/gen-sea-areas.mjs header.
set -eu
D="${1:?usage: fetch.sh <dir>}"
mkdir -p "$D/nws" "$D/ne" "$D/eccc" "$D/dwd"
curl -fsSL -o "$D/nws/mz.zip" https://www.weather.gov/source/gis/Shapefiles/WSOM/mz16ap26.zip
curl -fsSL -o "$D/nws/oz.zip" https://www.weather.gov/source/gis/Shapefiles/WSOM/oz16ap26.zip
curl -fsSL -o "$D/ne/land.zip" https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip
curl -fsSL -o "$D/eccc/water.zip" https://dd.weather.gc.ca/today/meteocode/geodata/version_6.15.0/MSC_Geography_Pkg_V6_15_0_Water_Unproj.zip
curl -fsSL -o "$D/dwd/kueste.json" "https://maps.dwd.de/geoserver/dwd/ows?service=WFS&version=2.0.0&request=GetFeature&typeNames=dwd:Warngebiete_Kueste&outputFormat=application/json&srsName=EPSG:4326"
unzip -oq "$D/nws/mz.zip" -d "$D/nws/mz"
unzip -oq "$D/nws/oz.zip" -d "$D/nws/oz"
unzip -oq "$D/ne/land.zip" -d "$D/ne/land"
unzip -oq "$D/eccc/water.zip" 'water_MarStdZone_coarse_unproj.*' -d "$D/eccc/w"
# V12 2026-11-02 (pass 2): MET Norway's sea forecast areas (textforecast 3.0 — the polygons travel with today's text;
# only the polygons and names are kept) and Met Éireann's sea-area warning polygons. api.met.no asks for an identifying
# User-Agent.
mkdir -p "$D/metno" "$D/metie"
curl -fsSL -A "VibeSDR gen-sea-areas (vibesdr.net)" -o "$D/metno/sea_en.json" https://api.met.no/weatherapi/textforecast/3.0/sea_en.json
curl -fsSL -A "VibeSDR gen-sea-areas (vibesdr.net)" -o "$D/metno/sea_no.json" https://api.met.no/weatherapi/textforecast/3.0/sea_no.json
curl -fsSL -o "$D/metie/sea-areas.geojson" https://opendata2.met.ie/opendata2/sea_area_polys/sea-areas.geojson
# AEMET's Meteoalerta zone delimitation (the coastal zones to 20 NM are one shapefile in it)
mkdir -p "$D/aemet"
curl -fsSL -o "$D/aemet/zonas.zip" https://www.aemet.es/documentos/es/eltiempo/prediccion/avisos/plan_meteoalerta/AEMET-meteoalerta-delimitacion-zonas.zip
unzip -oq "$D/aemet/zonas.zip" -d "$D/aemet"
