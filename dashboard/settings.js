const adminToken = localStorage.getItem("admin_token");
if (!adminToken) {
    window.location.href = "login.html";
}
const form = document.getElementById("password-form");
const resultDiv = document.getElementById("password-result");

form.addEventListener("submit", async function(event) {
    event.preventDefault();
    const currentPassword = document.getElementById("current-password").value;
    const newPassword = document.getElementById("new-password").value;
    const confirmPassword = document.getElementById("confirm-password").value;
    if (newPassword !== confirmPassword) {
        resultDiv.innerHTML = "<p>New passwords do not match.</p>";
        return;
    }
    const response = await fetch(
        "http://127.0.0.1:8000/admin/password",
        {
            method: "PATCH",
            headers: {
                "Content-Type": "application/json",
                "Authorization": `Bearer ${adminToken}`
            },
            body: JSON.stringify({
                current_password: currentPassword,
                new_password: newPassword
            })
        }
    );
    if (response.status === 401) {
        localStorage.removeItem("admin_token");
        window.location.href = "login.html";
        return;
    }
    const data = await response.json();
    if (data.success) {
        resultDiv.innerHTML = "<p>Password changed successfully.</p>";
        form.reset();
    } else {
        resultDiv.innerHTML = `<p>Error: ${data.reason}</p>`;
    }
});

function logout() {
    localStorage.removeItem("admin_token");
    window.location.href = "login.html";
}