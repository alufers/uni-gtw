---
sidebar_position: 3
---

# Advanced options

## Faster state updates when using other RF remotes

When another RF remote is paired to the blinds it might take some time after using it for the state of the covering to be picked up by the gateway (until it polls the controller for it's position). This is due to the fact that the blinds only send feedback to the remote that sent the command.

This can be remediated by registering the external remote as an "External remote" in uni-gtw. When that is done uni-gtw sniffs the packets sent by the remote and updates the state accordingly. 

To do that: Click "Edit" on the channel the external remote is paired to. Under "External remotes" press "Add remote". Then press any button (STOP is recommended) on the remote you wish to add. The gateway will pick up it's ID and add it to the list. Then save the channel.

TODO: screenshot


## Disabling bidirectional communication

Sometimes it might be possible that the commands sent to a motor are received, but the replies don't arrive reliably causing timeouts. This can be bypassed by disabling bidirectional communication. You can do that by editing a channel. When it is disabled the gateway does not wait for feedback from the motor, nor does it query or know it's position. This makes it work like a dumb wireless remote.
