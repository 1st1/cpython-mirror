import unittest
from test import support


class AsyncIter:
    def __init__(self, obj):
        self.obj = obj

    def __iter__(self):
        yield from self.obj

    __iter__.__async__ = True


class FutureLike:
    def __init__(self, value):
        self.value = value

    def __iter__(self):
        yield self.value

    __iter__.__async__ = True


class AsyncBadSyntaxTest(unittest.TestCase):
    def test_badsyntax_1(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async1

    def test_badsyntax_2(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async2

    def test_badsyntax_3(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async3

    def test_badsyntax_4(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async4

    def test_badsyntax_5(self):
        with self.assertRaisesRegex(SyntaxError, 'invalid syntax'):
            import test.badsyntax_async5

    def test_badsyntax_6(self):
        with self.assertRaisesRegex(
            SyntaxError, "'yield' inside async function"):

            import test.badsyntax_async6

    def test_badsyntax_7(self):
        with self.assertRaisesRegex(
            SyntaxError, "'yield from' inside async function"):

            import test.badsyntax_async7


class AsyncFunctionTest(unittest.TestCase):
    def test_func_1(self):
        async def foo():
            return 10
        self.assertTrue(bool(foo.__code__.co_flags & 0x80))
        self.assertTrue(bool(foo.__code__.co_flags & 0x20))
        self.assertTrue(bool(foo().gi_code.co_flags & 0x80))
        self.assertTrue(bool(foo().gi_code.co_flags & 0x20))
        try:
            next(foo())
        except StopIteration as ex:
            self.assertEqual(ex.args[0], 10)
        else:
            self.assertTrue(False)

        def bar(): pass
        self.assertFalse(bool(bar.__code__.co_flags & 0x80))

    def test_await_1(self):
        async def foo():
            await 1
        with self.assertRaisesRegex(TypeError, 'not iterable'):
            list(foo())

    def test_await_2(self):
        async def foo():
            await []
        with self.assertRaisesRegex(SystemError, 'not an async iterable'):
            list(foo())

    def test_await_3(self):
        async def foo():
            await AsyncIter([1, 2, 3])

        self.assertEqual(list(foo()), [1, 2, 3])

    def test_with_1(self):
        class Manager:
            def __init__(self, name):
                self.name = name

            async def __aenter__(self):
                await AsyncIter(['enter-1-' + self.name,
                                 'enter-2-' + self.name])
                return self

            async def __aexit__(self, *args):
                await AsyncIter(['exit-1-' + self.name,
                                 'exit-2-' + self.name])

                if self.name == 'B':
                    return True


        async def foo():
            async with Manager("A") as a, Manager("B") as b:
                await AsyncIter([('managers', a.name, b.name)])
                1/0

        f = foo()
        result = list(f)

        self.assertEqual(
            result, ['enter-1-A', 'enter-2-A', 'enter-1-B', 'enter-2-B',
                     ('managers', 'A', 'B'),
                     'exit-1-B', 'exit-2-B', 'exit-1-A', 'exit-2-A']
        )

    def test_with_2(self):
        class CM:
            def __aenter__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aexit__'):
            list(foo())

    def test_with_3(self):
        class CM:
            def __aexit__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aenter__'):
            list(foo())

    def test_with_4(self):
        class CM:
            def __enter__(self):
                pass

            def __exit__(self):
                pass

        async def foo():
            async with CM():
                pass

        with self.assertRaisesRegex(AttributeError, '__aexit__'):
            list(foo())

    def test_for_1(self):
        class AsyncIter:
            def __init__(self):
                self.i = 0

            def __aiter__(self):
                return self

            async def __anext__(self):
                self.i += 1

                if not (self.i % 10):
                    await FutureLike(self.i * 10)

                if self.i > 100:
                    raise StopAsyncIteration

                return self.i, self.i

        buffer = []

        async def foo():
            nonlocal buffer

            async for i1, i2 in AsyncIter():
                buffer.append(i1 + i2)

        yielded = list(foo())
        self.assertEqual(yielded, [i * 100 for i in range(1, 11)])
        self.assertEqual(buffer, [i*2 for i in range(1, 101)])

    def test_for_2(self):
        async def foo():
            async for i in ():
                pass

        with self.assertRaisesRegex(
                SystemError, "async for' requires an object with __aiter__"):

            list(foo())

    def test_for_3(self):
        class I:
            def __aiter__(self):
                return self

        async def foo():
            async for i in I():
                pass

        with self.assertRaisesRegex(
                SystemError,
                "iterators returned from __aiter__ must have __anext__"):

            list(foo())

    def test_for_4(self):
        class I:
            def __aiter__(self):
                return self

            def __anext__(self):
                return ()

        async def foo():
            async for i in I():
                pass

        with self.assertRaisesRegex(SystemError, "not an async iterable"):
            list(foo())

    def test_for_5(self):
        class I:
            def __aiter__(self):
                return self

            def __anext__(self):
                return 123

        async def foo():
            async for i in I():
                pass

        with self.assertRaisesRegex(TypeError, "is not iterable"):
            list(foo())


def test_main():
    support.run_unittest(AsyncBadSyntaxTest, AsyncFunctionTest)


if __name__=="__main__":
    test_main()
