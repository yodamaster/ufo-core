.. _using-env:

Environment variables
=====================

You can modify the run-time behaviour by setting environment variables:

.. envvar:: G_MESSAGES_DEBUG

    Controls the output of the library. By default nothing is printed on stdout.
    Set this to `all` to see debug output.

.. envvar:: UFO_PLUGIN_PATH

    Colon-separated list of paths in which plugin manager looks for additional
    task modules. The plugins are load with descending priority.

.. envvar:: UFO_KERNEL_PATH

    Colon-separated list of search paths for OpenCL kernel files.

.. envvar:: UFO_DEVICES

    Controls which OpenCL devices should be used. It works similar to the
    `CUDA_VISIBLE_DEVICES` environment variables, i.e. set it to `0,2` to choose
    the first and third device that's available.

.. envvar:: UFO_DEVICE_TYPE

    Controls which OpenCL device types should be considered for execution. The
    variable is a comma-separated list with strings being `cpu`, `gpu` and
    `acc`, i.e. to use both CPU and GPUs set `UFO_DEVICE_TYPE="cpu,gpu"`.
