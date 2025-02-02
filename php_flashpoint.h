#ifndef PHP_FLASHPOINT_H
#define PHP_FLASHPOINT_H

extern zend_module_entry flashpoint_module_entry;
#define phpext_example_ptr &flashpoint_module_entry

#define PHP_FLASHPOINT_VERSION "1.0"

#if defined(ZTS) && defined(COMPILE_DL_FLASHPOINT)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_FLASHPOINT_H */