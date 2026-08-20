# pylibseekdb compatibility package

`pylibseekdb` was renamed to `seekdb` in version 1.4.0. This final
compatibility release installs `seekdb==1.4.0` and preserves the public
top-level import temporarily:

```python
import pylibseekdb  # deprecated
```

Applications should migrate to:

```python
import seekdb
```

The former internal extension path `pylibseekdb.pylibseekdb` is not part of
the compatibility contract.
