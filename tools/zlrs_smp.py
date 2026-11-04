# SPDX-License-Identifier: Apache-2.0
"""Application SMP groups using the standard smpclient serial transport."""

import asyncio


def request_type(group, fields=None):
    """Build command 0 for an application group, including its native error types."""
    from pydantic import ConfigDict
    from smp import error, message

    class GroupErrorV1(error.ErrorV1):
        _GROUP_ID = group

    class GroupErrorV2(error.ErrorV2[error.MGMT_ERR]):
        _GROUP_ID = group

    class Response(message.ReadResponse if fields is None else message.WriteResponse):
        _GROUP_ID = group
        _COMMAND_ID = 0
        model_config = ConfigDict(extra="allow", frozen=True)
        result: int

    class Request(message.ReadRequest if fields is None else message.WriteRequest):
        _GROUP_ID = group
        _COMMAND_ID = 0
        _Response = Response
        _ErrorV1 = GroupErrorV1
        _ErrorV2 = GroupErrorV2
        model_config = ConfigDict(extra="allow", frozen=True)

    return Request(**(fields or {}))


def request(port, group, fields=None, timeout=10.0, baud=115200):
    """Read command 0, or write its CBOR fields; preserve operation errno results."""
    from smpclient import SMPClient
    from smpclient.generics import success
    from smpclient.transport.serial import SMPSerialTransport

    async def exchange():
        async with SMPClient(SMPSerialTransport(baudrate=baud), port, timeout_s=timeout) as client:
            response = await client.request(request_type(group, fields))
            if not success(response):
                raise ValueError(f"SMP group {group} rejected the request: {response}")
            return {"result": response.result, **(response.model_extra or {})}

    return asyncio.run(exchange())
