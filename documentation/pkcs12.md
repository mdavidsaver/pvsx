# PKCS#12 files in brief

The following is based on a reading of [RFC7292](https://datatracker.ietf.org/doc/html/rfc7292) as of July 2014.
Also on observation of OpenSSL circa 3.1 and keytool circa Java 17.

End users do not need to know this.

## File Structure

Each `PKCS#12` file contains a list of `AuthenticatedSafe` entries (aka. "Safes").

```
PKCS#12 file
  AuthenticatedSafe (unencrypted)
    ...
  AuthenticatedSafe (password encrypted)
    ...
  AuthenticatedSafe (public key encrypted)
```

Each Safe may be: unencrypted, encrypted with a password, or encrypted with a public key.

Each Safe contains a list of `SafeBag` entries (aka. "Bags").

```
PKCS#12 file
  AuthenticatedSafe
    *Bag
      attributes...
      value
```

Each Bag has an a list of "Attributes" and a "value".

RFC7292 defines two attributes `friendlyName` and `localKeyId`.
Additionally, Java defines another `oracle-jdk-trustedkeyusage` or `ORACLE_TrustedKeyUsage`.

```
PKCS#12 file
  AuthenticatedSafe
    keyBag              (unencrypted private key)
    pkcs8ShroudedKeyBag (encrypted private key)
    certBag             (certificate, usually X509)
    crlBag              (certificate revocation list, usually X509CRL)
    secretBag           (arbitrary encrypted bytes)
    safeContentsBag
      ... recursive list of Bags
```

RFC7292 defines 6 types of Bag, and leaves open the possibility of more.


## Bag Attributes

`friendlyName` is a string labeling a Bag.
Java keytool uses these (via. `-alias`) to distinguish multiple private keys within one file.
OpenSSL ignores them, and gets confused if multiple private keys are present.

`localKeyId` is meant to identifies pairs of private key and certificate.

`oracle-jdk-trustedkeyusage` has the same value as the X509 `extendedKeyUsage` extension.

Released version of OpenSSL as of 3.1 circa Aug. 2023 [do not understand](https://github.com/openssl/openssl/issues/6684) `oracle-jdk-trustedkeyusage`.
This is feature [planned for 3.2](https://github.com/openssl/openssl/pull/19025).

TODO: keytool has been observed setting this to "6".  OpenSSL 3.2 set `anyExtendedKeyUsage`, aka. 1.

## File Structure as Observed

The structures of files created by `openssl pkcs12` and `keytool` are almost identical.

For example, a file with a certificate/key pair, and an associated CA certificate is structured like:

```
PKCS#12
  AuthenticatedSafe (unencrypted)
    pkcs8ShroudedKeyBag
      attributes
        friendlyName = "my:cert:name"    (Java only)
        localKeyId = ...                 (value will match the associated keyBag or pkcs8ShroudedKeyBag
      value = private key...
  AuthenticatedSafe (encrypted)
    certBag
      attributes
        friendlyName = "my:cert:name"    (Java only)
        localKeyId = ...                 (value will match the associated certBag
      value = X509 certificate
    certBag
      attributes
        friendlyName = "my:ca"           (Java only)
        oracle-jdk-trustedkeyusage = ... (Java only)
      value = X509 certificate
```

Notes...

This structure leaves the friendlyName (aka `-alias`) and localKeyId associated with a private key unencrypted in all cases.

Java keytool has been observed (after an `-importcert`) to put almost two certBag entries with the same certificate.
One with the friendlyName from `-alias` and `oracle-jdk-trustedkeyusage` set,
and a second with friendlyName set to the distinguishing name (eg. `CN=foo,O=bar`) and no `oracle-jdk-trustedkeyusage`.
keytool seems to ignore any entries without `oracle-jdk-trustedkeyusage`, but OpenSSL reads them.
