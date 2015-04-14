import unittest
from test import support


class AsyncIter:
    def __init__(self, obj):
        self.obj = obj

    def __iter__(self):
        yield from self.obj

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
        with self.assertRaisesRegexp(TypeError, 'not iterable'):
            list(foo())

    def test_await_2(self):
        async def foo():
            await []
        with self.assertRaisesRegexp(SystemError, 'not an async iterable'):
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


def test_main():
    support.run_unittest(AsyncBadSyntaxTest, AsyncFunctionTest)


if __name__=="__main__":
    test_main()
