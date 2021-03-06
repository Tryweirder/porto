#!/usr/bin/python

import os
import time
import porto
from test_common import *

c = porto.Connection()

a = c.Run('a')

ExpectProp(a, 'labels', '')

ExpectEq(Catch(c.SetLabel, 'a', '!', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'TEST.!', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'TEST./', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'a.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'A.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'A' * 17 + '.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'AAAAAAAA.' + 'a' * 150, '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'AAAAAAAA.a', ' '), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'AAAAAAAA.a', 'a' * 300), porto.exceptions.InvalidLabel)
ExpectEq(Catch(c.SetLabel, 'a', 'PORTO.a', '.'), porto.exceptions.InvalidLabel)

c.SetLabel('a', 'A' * 16 + '.a', '/')
c.SetLabel('a', 'A' * 16 + '.a', '.')
c.SetLabel('a', 'A' * 16 + '.a', '')

c.SetLabel('a', 'A' * 16 + '.' + 'a' * 111, '.')
c.SetLabel('a', 'A' * 16 + '.' + 'a' * 111, '')

for i in range(100):
    c.SetLabel('a', 'TEST.' + str(i), '.')

ExpectEq(Catch(c.SetLabel, 'a', 'TEST.a', '.'), porto.exceptions.ResourceNotAvailable)

for i in range(100):
    c.SetLabel('a', 'TEST.' + str(i), '')

ExpectProp(a, 'labels', '')

ExpectEq(Catch(a.GetLabel, 'TEST.a'), porto.exceptions.LabelNotFound)
ExpectProp(a, 'labels', '')
ExpectEq(Catch(a.GetProperty, 'TEST.a'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, 'labels[TEST.a]'), porto.exceptions.LabelNotFound)
ExpectEq(c.FindLabel('TEST.a'), [])
ExpectEq(Catch(c.WaitLabels, ['***'], ['TEST.*'], timeout=0), porto.exceptions.WaitContainerTimeout)
ExpectEq(Catch(c.WaitLabels, ['a'], ['TEST.*'], timeout=0), porto.exceptions.WaitContainerTimeout)

c.SetLabel('a', 'TEST.a', '.')
ExpectEq(a.GetLabel('TEST.a'), '.')
ExpectProp(a, 'labels', 'TEST.a: .')
ExpectProp(a, 'labels[TEST.a]', '.')
ExpectProp(a, 'TEST.a', '.')

w = c.WaitLabels(['***'], ['TEST.*'], timeout=0)
ExpectEq(w['name'], 'a')
ExpectEq(w['state'], 'meta')
ExpectEq(w['label'], 'TEST.a')
ExpectEq(w['value'], '.')

ExpectEq(c.WaitLabels(['a'], ['TEST.*'], timeout=0)['name'], 'a')

ExpectEq(c.FindLabel('TEST.a'), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])
ExpectEq(c.FindLabel('*.*'), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])
ExpectEq(c.FindLabel('TEST.b'), [])
ExpectEq(c.FindLabel('TEST.a', mask="b"), [])
ExpectEq(c.FindLabel('TEST.a', state="stopped"), [])
ExpectEq(c.FindLabel('TEST.a', value="2"), [])
ExpectEq(c.FindLabel('TEST.a*', mask="a*", state="meta", value="."), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])

c.SetLabel('a', 'TEST.a', '')
ExpectProp(a, 'labels', '')
ExpectEq(Catch(a.GetProperty, 'TEST.a'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, 'labels[TEST.a]'), porto.exceptions.LabelNotFound)
ExpectEq(c.FindLabel('TEST.a'), [])
ExpectEq(Catch(c.WaitLabels, ['***'], ['TEST.*'], timeout=0), porto.exceptions.WaitContainerTimeout)

ExpectEq(Catch(a.SetLabel, 'TEST.a', '.', 'N'), porto.exceptions.LabelNotFound)

a.SetLabel('TEST.a', 'N', '')
ExpectProp(a, 'TEST.a', 'N')

a.SetLabel('TEST.a', 'Y', 'N')
ExpectProp(a, 'TEST.a', 'Y')

ExpectEq(Catch(a.SetLabel, 'TEST.a', 'Y', 'N'), porto.exceptions.Busy)
ExpectProp(a, 'TEST.a', 'Y')

a.SetLabel('TEST.a', '', 'Y')
ExpectEq(Catch(a.GetProperty, 'TEST.a'), porto.exceptions.LabelNotFound)


ExpectEq(Catch(a.IncLabel, 'TEST.a', add=0), porto.exceptions.LabelNotFound)
a.SetLabel('TEST.a', 'a')
ExpectEq(Catch(a.IncLabel, 'TEST.a'), porto.exceptions.InvalidValue)
a.SetLabel('TEST.a', '')
ExpectEq(a.IncLabel('TEST.a'), 1)
ExpectEq(a.IncLabel('TEST.a', 2), 3)
ExpectEq(a.IncLabel('TEST.a', -2), 1)
ExpectEq(Catch(a.IncLabel, 'TEST.a', 2**63-1), porto.exceptions.InvalidValue)
ExpectEq(a.IncLabel('TEST.a', 2**63-2), 2**63-1)
ExpectEq(a.IncLabel('TEST.a', -(2**63-1)), 0)
ExpectEq(a.IncLabel('TEST.a', -1), -1)
ExpectEq(Catch(a.IncLabel, 'TEST.a', -(2**63)), porto.exceptions.InvalidValue)
ExpectEq(a.IncLabel('TEST.a', 1), 0)
ExpectEq(a.IncLabel('TEST.a', -(2**63)), -(2**63))
a.SetLabel('TEST.a', str(2**64))
ExpectEq(Catch(a.IncLabel, 'TEST.a', -1), porto.exceptions.InvalidValue)
a.SetLabel('TEST.a', str(-(2**64)))
ExpectEq(Catch(a.IncLabel, 'TEST.a', 1), porto.exceptions.InvalidValue)
a.SetLabel('TEST.a', '')


b = c.Run('a/b')

a['TEST.a'] = 'a'
b['TEST.b'] = 'b'

ExpectProp(a, 'TEST.a', 'a')
ExpectProp(b, '.TEST.a', 'a')
ExpectProp(b, 'TEST.b', 'b')
ExpectEq(b.GetLabel('.TEST.a'), 'a')

ExpectEq(Catch(b.GetProperty, 'TEST.a'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, 'TEST.b'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, '.TEST.b'), porto.exceptions.LabelNotFound)

ExpectEq(c.FindLabel('TEST.a'), [{'name':'a', 'label':'TEST.a', 'value':'a', 'state':'meta'}])
ExpectEq(c.FindLabel('.TEST.a'), [{'name':'a', 'label':'.TEST.a', 'value':'a', 'state':'meta'}, {'name':'a/b', 'label':'.TEST.a', 'value':'a', 'state':'meta'}])
ExpectEq(c.FindLabel('TEST.b'), [{'name':'a/b', 'label':'TEST.b', 'value':'b', 'state':'meta'}])
ExpectEq(c.FindLabel('.TEST.*'), [])

a.Destroy()

# Volume Labels

ExpectEq(c.GetVolumes(labels=['TEST.a']), [])
ExpectEq(c.GetVolumes(labels=['TEST.b']), [])

v = c.CreateVolume(labels='TEST.a: a; TEST.b: b')
ExpectEq(v.GetProperty('labels'), 'TEST.a: a; TEST.b: b')
ExpectEq(v.GetLabel('TEST.a'), 'a')
ExpectEq(v.GetLabel('TEST.b'), 'b')
ExpectEq(len(c.GetVolumes(labels=['TEST.a'])), 1)
ExpectEq(len(c.GetVolumes(labels=['TEST.b'])), 1)
ReloadPortod()
ExpectEq(v.GetProperty('labels'), 'TEST.a: a; TEST.b: b')
v.Destroy()

v = c.CreateVolume()

ExpectEq(v.GetProperty('labels'), '')

ExpectEq(Catch(v.SetLabel, '!', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'TEST.!', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'TEST./', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'a.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'A.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'A' * 17 + '.a', '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'AAAAAAAA.' + 'a' * 150, '.'), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'AAAAAAAA.a', ' '), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'AAAAAAAA.a', 'a' * 300), porto.exceptions.InvalidLabel)
ExpectEq(Catch(v.SetLabel, 'PORTO.a', '.'), porto.exceptions.InvalidLabel)

v.SetLabel('A' * 16 + '.a', '.')
v.SetLabel('A' * 16 + '.a', '')

v.SetLabel('A' * 16 + '.' + 'a' * 111, '.')
v.SetLabel('A' * 16 + '.' + 'a' * 111, '')

for i in range(100):
    v.SetLabel('TEST.' + str(i), '.')

ExpectEq(Catch(v.SetLabel, 'TEST.a', '.'), porto.exceptions.ResourceNotAvailable)

for i in range(100):
    v.SetLabel('TEST.' + str(i), '')

ExpectEq(v.GetProperty('labels'), '')

ExpectEq(Catch(v.GetLabel, 'TEST.a'), porto.exceptions.LabelNotFound)

c.SetVolumeLabel(v.path, 'TEST.a', '/')
ExpectEq(v.GetLabel('TEST.a'), '/')
ExpectEq(v.GetProperty('labels'), 'TEST.a: /')
v.SetLabel('TEST.b', 'b')
ExpectEq(v.GetProperty('labels'), 'TEST.a: /; TEST.b: b')

v.Destroy()
