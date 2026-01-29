# YX Camera

This repository contains an ESP-IDF project for AWS-enabled devices intended to
function as security cameras in Proyecto Vuelta's vending machines. The devkit
used during development is 
[Waveshare's ESP32-P4-NANO](https://www.waveshare.com/esp32-p4-nano.htm) and code
was written for ESP-IDF v5.5.

# Getting started
Before using this repository, you will need to install:
* [ESP-IDF v5.5.2](https://github.com/espressif/esp-idf/releases/tag/v5.5.2)
* [AWS CLI version 2](https://aws.amazon.com/cli/)
## Installing and configuring ESP-IDF v5.5.2
The easiest way to install ESP-IDF is using the [official VSCode extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension). You can follow the instructions
in the [readme](https://github.com/espressif/vscode-esp-idf-extension) from the GitHub
repository for the extension. Be sure to select **v5.5.2** under "*Select ESP-IDF version*"
when configuring your install.

If you already have an ESP-IDF installation in your system that is not v5.5.2, you can follow
the instructions in the [official ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/versions.html#updating-to-stable-release) to update to v5.5.2.

When building the project, make sure that `esp32p4` is [selected as target](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/idf-py.html#select-the-target-chip-set-target). If not, the build will fail because the necessary
dependencies are only available for the ESP32 P4 chip. For more information about building
and flashing the firmware, visit the GitHub Wiki for this repository.

## Installing and configuring AWS CLI version 2
Please follow the [official installation guide for AWS CLI version 2](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) and log in using the `aws login` command. It
is adviced (but not mandatory) to pass the `--profile` argument for this command in order to
create a new AWS CLI profile instead of using the default.

Some scripts in this repository will require that you assume a certain AWS IAM role when executing
them. The easiest way to assume a role via the AWS CLI is to configure a new profile for this.
To do so, open the `config` file (located at `~/.aws/config` on Linux or macOS, or at `C:\Users\USERNAME\.aws\config` on Windows) and create a new profile with a `role_arn` line
that matches the desired role's ARN:
```toml
[profile <new profile name>]
role_arn       = arn.aws.iam::<AWS Account number>:role/<IAM Role name>
source_profile = <Base profile>
```
This will use the credentials for the profile `<Base profile>` to assume the role 
`<IAM Role name>` when running AWS CLI commands with `--profile <new profile name>`. If
you don't want to pass the `--profile` argument each time, you can also set the environment
variable `AWS_PROFILE` to the name of the profile you want to use.
