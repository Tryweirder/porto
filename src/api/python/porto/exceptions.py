from . import rpc_pb2

class PortoException(Exception):
    EID = None
    __TYPES__ = {}

    @classmethod
    def _Init(cls):
        for eid, err in rpc_pb2._EERROR.values_by_number.items():
            e_class = type(err.name, (cls,), {'EID': eid})
            cls.__TYPES__[eid] = e_class
            globals()[err.name] = e_class

    @classmethod
    def Create(cls, eid, msg):
        e_class = cls.__TYPES__.get(eid)
        if e_class is not None:
            return e_class(msg)
        return UnknownError(msg)

    def __str__(self):
        return '%s: %s' % (self.__class__.__name__, self.args)


class WaitContainerTimeout(PortoException):
    pass


PortoException._Init()

EError = PortoException
PermissionError = Permission
UnknownError = Unknown
