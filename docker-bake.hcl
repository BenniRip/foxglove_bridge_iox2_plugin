variable "ROS_DISTRO" {
  default = "jazzy"
}

target "_common" {
  context    = "."
  dockerfile = "docker/Dockerfile"
  args = {
    ROS_DISTRO = ROS_DISTRO
  }
}

group "default" {
  targets = ["lint", "test", "smoke", "deb"]
}

target "dev" {
  inherits = ["_common"]
  target   = "dev"
  tags     = ["foxglove-bridge-iox2-plugin-dev:${ROS_DISTRO}"]
  output   = ["type=docker"]
}

target "lint" {
  inherits = ["_common"]
  target   = "lint"
  output   = ["type=cacheonly"]
}

# Rerun even when cached: they also depend on shared memory and timing.
target "test" {
  inherits        = ["_common"]
  target          = "test"
  no-cache-filter = ["test"]
  output          = ["type=cacheonly"]
}

target "smoke" {
  inherits        = ["_common"]
  target          = "smoke"
  no-cache-filter = ["smoke"]
  output          = ["type=cacheonly"]
}

target "deb" {
  inherits = ["_common"]
  target   = "deb"
  output   = ["type=local,dest=dist"]
}
