<!--
If you are adding support for a new generic xpad controller it is sufficient
to update the xpad_table[] array.
The type will be auto-detected in xpad_probe().
Updating the xpad_device[] array is only needed if the controller requires
additional flags like DANCEPAD_MAP_CONFIG to work.

If you are updating any of the above tables, make sure you keep the sorted!

# Attribution

To get attribution for your work when this goes upstream, make sure to add
the following line at the end of YOUR COMMIT MESSAGE:

Signed-off-by: Random J Developer <random@developer.example.org>

You must use real name and a real email address as per:
https://www.kernel.org/doc/html/v4.10/process/submitting-patches.html#sign-your-work-the-developer-s-certificate-of-origin

If you skip this line, your commit will be sent upstream anonymously.
 -->