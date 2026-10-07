FROM gcr.io/oss-fuzz-base/base-builder:v1@sha256:8b4a73d83374b298a0a771eeec9a1d44ceff3416a678b7d7946303389d022aca

COPY . $SRC/tasksmack
WORKDIR $SRC/tasksmack
# Header-only dependencies of the config/theme fuzz targets, fetched at image build so build.sh
# needs no network.
ENV FUZZ_DEPS_DIR=$SRC/fuzz-deps
RUN .clusterfuzzlite/fetch-deps.sh
COPY .clusterfuzzlite/build.sh $SRC/build.sh
