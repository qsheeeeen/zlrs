# SPDX-License-Identifier: Apache-2.0
"""Application SMP groups using the standard smpclient serial transport."""

import asyncio


def request(port, group, fields=None, timeout=10.0, baud=115200):
    """Read command 0, or write its CBOR fields; preserve operation errno results."""
    from pydantic import ConfigDict
    from smp import error, message
    from smpclient import SMPClient
    from smpclient.generics import success
    from smpclient.transport.serial import SMPSerialTransport

    class Response(message.ReadResponse if fields is None else message.WriteResponse):
        _GROUP_ID = group
        _COMMAND_ID = 0
        model_config = ConfigDict(extra="allow", frozen=True)
        result: int

    class Request(message.ReadRequest if fields is None else message.WriteRequest):
        _GROUP_ID = group
        _COMMAND_ID = 0
        _Response = Response
        _ErrorV1 = error.ErrorV1
        _ErrorV2 = error.ErrorV2[error.MGMT_ERR]
        model_config = ConfigDict(extra="allow", frozen=True)

    async def exchange():
        async with SMPClient(SMPSerialTransport(baudrate=baud), port, timeout_s=timeout) as client:
            response = await client.request(Request(**(fields or {})))
            if not success(response):
                raise ValueError(f"SMP group {group} rejected the request: {response}")
            return {"result": response.result, **(response.model_extra or {})}

    return asyncio.run(exchange())
