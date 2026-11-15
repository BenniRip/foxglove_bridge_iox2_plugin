# syntax=docker/dockerfile:1
# dev: ROS 2, iceoryx2 (outside ROS, in /opt/iceoryx2) and the bridge with the plugin API.
# The other stages are run by docker-bake.hcl.
ARG ROS_DISTRO=jazzy
FROM ros:${ROS_DISTRO}-ros-base AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get -y dist-upgrade && apt-get install -y --no-install-recommends \
      curl git python3-bloom fakeroot dpkg-dev debhelper \
    && rm -rf /var/lib/apt/lists/*

# Only this stage needs Rust; dev gets the installed libraries.
FROM base AS iceoryx2
ARG ICEORYX2_VERSION=v0.10.0
# Must match the flatbuffers version iceoryx2 is built against (see its Cargo.toml).
ARG FLATBUFFERS_VERSION=v25.12.19

# Pinned: with Rust 1.100, iceoryx2's C++ build does not find its generated C header.
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --default-toolchain 1.99.1
ENV PATH=/root/.cargo/bin:${PATH}

RUN git clone --depth 1 --branch ${FLATBUFFERS_VERSION} https://github.com/google/flatbuffers.git /tmp/flatbuffers \
    && cmake -S /tmp/flatbuffers -B /tmp/flatbuffers/build \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/opt/iceoryx2 \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
         -DFLATBUFFERS_BUILD_TESTS=OFF \
    && cmake --build /tmp/flatbuffers/build -j"$(nproc)" \
    && cmake --install /tmp/flatbuffers/build

RUN git clone --depth 1 --branch ${ICEORYX2_VERSION} https://github.com/eclipse-iceoryx/iceoryx2.git /tmp/iceoryx2 \
    && cmake -S /tmp/iceoryx2 -B /tmp/iceoryx2/build \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/opt/iceoryx2 \
         -DCMAKE_PREFIX_PATH=/opt/iceoryx2 \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
         -DBUILD_EXAMPLES=OFF \
         -DIOX2_FEATURE_FLATBUFFERS=ON \
         -DIOX2_DEPENDENCY_USE_SYSTEM_FLATBUFFERS=ON \
    && cmake --build /tmp/iceoryx2/build -j"$(nproc)" \
    && cmake --install /tmp/iceoryx2/build

# Built as a .deb like a released bridge.
FROM base AS bridge
ARG ROS_DISTRO
ARG FOXGLOVE_SDK_REPO=https://github.com/BenniRip/foxglove-sdk.git
# Commit of the bridge with the channel source plugin API (BenniRip/foxglove-sdk#3).
ARG FOXGLOVE_SDK_REF=1f52ea81302a46bc0253a39db8d9cdac463a1e6a
RUN git init -q /tmp/foxglove-sdk \
    && git -C /tmp/foxglove-sdk fetch -q --depth 1 ${FOXGLOVE_SDK_REPO} ${FOXGLOVE_SDK_REF} \
    && git -C /tmp/foxglove-sdk checkout -q FETCH_HEAD \
    && apt-get update \
    && rosdep update --rosdistro ${ROS_DISTRO} \
    && rosdep install -y --from-paths /tmp/foxglove-sdk/ros/src/foxglove_bridge --ignore-src \
         --rosdistro ${ROS_DISTRO} \
    && cd /tmp/foxglove-sdk/ros \
    && . /opt/ros/${ROS_DISTRO}/setup.sh \
    && DEB_BUILD_OPTIONS=nocheck make deb-bridge \
    && mkdir /opt/debs && cp dist/ros-${ROS_DISTRO}-foxglove-bridge_*.deb /opt/debs/

FROM base AS dev
ARG ROS_DISTRO
RUN apt-get update && apt-get install -y --no-install-recommends clang-format \
    && rm -rf /var/lib/apt/lists/*

COPY --from=iceoryx2 /opt/iceoryx2 /opt/iceoryx2
ENV CMAKE_PREFIX_PATH=/opt/iceoryx2
ENV PATH=/opt/iceoryx2/bin:${PATH}

# Kept for the deb stage.
COPY --from=bridge /opt/debs /opt/debs
RUN apt-get update && apt-get install -y /opt/debs/*.deb \
    && rosdep update --rosdistro ${ROS_DISTRO} \
    && rm -rf /var/lib/apt/lists/*

COPY package.xml /tmp/plugin/package.xml
RUN apt-get update \
    && rosdep install -y --from-paths /tmp/plugin --ignore-src --rosdistro ${ROS_DISTRO} \
    && rm -rf /var/lib/apt/lists/* /tmp/plugin

# For the devcontainer, which runs as ubuntu.
RUN mkdir -p /ws/src && chown -R ubuntu:ubuntu /ws \
    && echo "source /opt/ros/${ROS_DISTRO}/setup.bash" >> /etc/bash.bashrc

SHELL ["/bin/bash", "-c"]
WORKDIR /ws

FROM dev AS lint
RUN --mount=type=bind,target=/ws/src/foxglove_bridge_iox2_plugin \
    cd src/foxglove_bridge_iox2_plugin \
    && find src include tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run -Werror

FROM dev AS build
RUN --mount=type=bind,target=/ws/src/foxglove_bridge_iox2_plugin \
    source /opt/ros/${ROS_DISTRO}/setup.bash \
    && colcon build --cmake-args -DCMAKE_COMPILE_WARNING_AS_ERROR=ON

FROM build AS test
# Loopback and /dev/shm are enough.
RUN --mount=type=bind,target=/ws/src/foxglove_bridge_iox2_plugin --network=none \
    source /opt/ros/${ROS_DISTRO}/setup.bash && colcon test && colcon test-result --verbose

FROM build AS smoke
RUN --mount=type=bind,target=/ws/src/foxglove_bridge_iox2_plugin --network=none \
    source install/setup.bash \
    && src/foxglove_bridge_iox2_plugin/tests/smoke_test.sh /ws/build/foxglove_bridge_iox2_plugin

# A copy, since bloom writes debian/ into the source.
FROM dev AS deb-build
COPY . /ws/src/foxglove_bridge_iox2_plugin
RUN cd src/foxglove_bridge_iox2_plugin \
    && bloom-generate rosdebian --ros-distro ${ROS_DISTRO} \
    && DEB_BUILD_OPTIONS=nocheck fakeroot debian/rules binary \
    && mkdir /dist && mv ../ros-${ROS_DISTRO}-*.deb /dist/ && cp /opt/debs/*.deb /dist/

# The plugin's .deb and the bridge .deb it was built against.
FROM scratch AS deb
COPY --from=deb-build /dist/ /
